"""
mock_ws_server.py — Minimal async WebSocket server that speaks the P5 Aqua-HMET
wire protocol for browser/client testing.

Can be used standalone:
  python test/mock_ws_server.py [--port 8765]

Or imported by test_p5_protocol.py to spin up an in-process server.

Wire protocol implemented:
  Outbound binary frames  : net_ws_dp_frame_t (16 bytes, little-endian)
  Outbound text (JSON)    : hello, event, state, resync_complete
  Inbound JSON commands   : start, abort, zero, state, hello

Usage (standalone):
  pip install websockets
  python test/mock_ws_server.py
  # then open http://localhost:8765 in the placeholder SPA (update WS URL)
"""

import asyncio
import json
import struct
import math
import argparse
import logging
from typing import Set

log = logging.getLogger("mock_ws_server")

# ---------------------------------------------------------------------------
# Wire protocol constants — mirror net_comms_protocol.h
# ---------------------------------------------------------------------------

PROTOCOL_VERSION     = 1
FW_VERSION           = "1.0.0-mock"

WS_FRAME_TYPE_DP     = 0x01          # binary DataPoint frame marker
WS_FRAME_SIZE        = 16            # bytes
WS_FRAME_FMT         = "<BBHfff"     # LE: frame_type, electrode, idx, E_mV, I_uA, RE_mV

CSV_HEADER           = "electrode,idx,E_mV,I_uA,RE_mV\r\n"
CSV_ROW_FMT          = "{electrode},{idx},{E_mV:.4f},{I_uA:.4f},{RE_mV:.4f}\r\n"

EVENT_STARTED        = "scan_started"
EVENT_COMPLETE       = "scan_complete"
EVENT_ABORTED        = "scan_aborted"
EVENT_ERROR          = "scan_error"

# ---------------------------------------------------------------------------
# Synthetic cell model: Gaussian DPV peaks (same as echem_core test model)
# ---------------------------------------------------------------------------

MOCK_PEAKS = [
    {"E_peak": -700.0, "I_peak": 45.0, "sigma": 60.0},   # Cd²⁺  ~ -0.7 V
    {"E_peak": -400.0, "I_peak": 30.0, "sigma": 50.0},   # Pb²⁺  ~ -0.4 V
    {"E_peak":    0.0, "I_peak": 20.0, "sigma": 55.0},   # Cu²⁺  ~  0.0 V
]

def synthetic_current(E_mV: float) -> float:
    """Gaussian-peak DPV response (µA) for a given potential (mV)."""
    I = 0.0
    for pk in MOCK_PEAKS:
        I += pk["I_peak"] * math.exp(-0.5 * ((E_mV - pk["E_peak"]) / pk["sigma"]) ** 2)
    # Add ~0.5 µA baseline noise (deterministic, no random — reproducible)
    I += 0.5 * math.sin(E_mV * 0.01)
    return I


def encode_dp_frame(electrode: int, idx: int,
                    E_mV: float, I_uA: float, RE_mV: float) -> bytes:
    """Pack a binary DataPoint frame (16 bytes, little-endian)."""
    return struct.pack(WS_FRAME_FMT,
                       WS_FRAME_TYPE_DP, electrode, idx,
                       E_mV, I_uA, RE_mV)


def decode_dp_frame(data: bytes) -> dict:
    """Unpack a 16-byte binary DataPoint frame into a dict.
    Returns None if the frame is malformed.
    """
    if len(data) != WS_FRAME_SIZE:
        return None
    frame_type, electrode, idx, E_mV, I_uA, RE_mV = struct.unpack(WS_FRAME_FMT, data)
    if frame_type != WS_FRAME_TYPE_DP:
        return None
    return {
        "frame_type": frame_type,
        "electrode":  electrode,
        "idx":        idx,
        "E_mV":       E_mV,
        "I_uA":       I_uA,
        "RE_mV":      RE_mV,
    }


# ---------------------------------------------------------------------------
# Server state
# ---------------------------------------------------------------------------

class MockServer:
    def __init__(self):
        self.clients: Set = set()
        self.scan_buf: list = []   # list of dicts (for resync)
        self.state: str = "IDLE"   # IDLE / RUNNING / COMPLETE

    def _make_hello(self) -> str:
        return json.dumps({
            "t":     "hello",
            "fw":    FW_VERSION,
            "proto": PROTOCOL_VERSION,
        })

    def _make_event(self, name: str, **kwargs) -> str:
        msg = {"t": "event", "name": name}
        msg.update(kwargs)
        return json.dumps(msg)

    def _make_state(self) -> str:
        return json.dumps({
            "t":     "state",
            "state": self.state,
            "pts":   len(self.scan_buf),
        })

    def _make_resync_complete(self) -> str:
        return json.dumps({
            "t":    "resync_complete",
            "pts":  len(self.scan_buf),
            "state": self.state,
        })

    async def _broadcast_text(self, msg: str):
        if self.clients:
            await asyncio.gather(
                *[ws.send(msg) for ws in self.clients],
                return_exceptions=True
            )

    async def _broadcast_binary(self, data: bytes):
        if self.clients:
            await asyncio.gather(
                *[ws.send(data) for ws in self.clients],
                return_exceptions=True
            )

    async def _run_synthetic_scan(self, electrode: int = 1,
                                   e_begin: float = -900.0,
                                   e_end: float = 200.0,
                                   e_step: float = 10.0):
        """Generate and broadcast a synthetic DPV scan."""
        self.scan_buf.clear()
        self.state = "RUNNING"
        await self._broadcast_text(self._make_event(EVENT_STARTED,
                                                    mode="DPV",
                                                    electrode=electrode))

        E = e_begin
        idx = 0
        direction = 1 if e_end > e_begin else -1
        while (direction > 0 and E <= e_end) or (direction < 0 and E >= e_end):
            I_uA  = synthetic_current(E)
            RE_mV = E - 5.0   # mock RE offset

            frame = encode_dp_frame(electrode, idx, E, I_uA, RE_mV)
            await self._broadcast_binary(frame)

            self.scan_buf.append({
                "electrode": electrode, "idx": idx,
                "E_mV": E, "I_uA": I_uA, "RE_mV": RE_mV,
            })

            E += direction * e_step
            idx += 1
            await asyncio.sleep(0.002)   # 2 ms — fast mock

        self.state = "COMPLETE"
        await self._broadcast_text(self._make_event(EVENT_COMPLETE))
        # Auto-reset
        await asyncio.sleep(0.1)
        self.state = "IDLE"

    async def handle_client(self, websocket):
        """Handle one WS client connection."""
        self.clients.add(websocket)
        log.info("Client connected (%d total)", len(self.clients))

        try:
            # Send hello + resync
            await websocket.send(self._make_hello())
            for entry in self.scan_buf:
                frame = encode_dp_frame(**entry)
                await websocket.send(frame)
            await websocket.send(self._make_resync_complete())

            async for message in websocket:
                if isinstance(message, bytes):
                    continue   # server doesn't expect binary from client

                try:
                    cmd = json.loads(message)
                except json.JSONDecodeError:
                    log.warning("Bad JSON from client: %r", message[:80])
                    continue

                action = cmd.get("cmd", "")

                if action == "start":
                    if self.state == "IDLE":
                        params = cmd.get("params", {})
                        electrode = int(cmd.get("electrode", 1))
                        asyncio.get_event_loop().create_task(
                            self._run_synthetic_scan(
                                electrode=electrode,
                                e_begin=float(params.get("e_begin_mV", -900.0)),
                                e_end=float(params.get("e_end_mV", 200.0)),
                                e_step=float(params.get("e_step_mV", 10.0)),
                            )
                        )
                    else:
                        await websocket.send(json.dumps({
                            "t": "error",
                            "msg": "scan already running",
                        }))

                elif action == "abort":
                    if self.state == "RUNNING":
                        self.state = "IDLE"
                        await self._broadcast_text(
                            self._make_event(EVENT_ABORTED))

                elif action == "state":
                    await websocket.send(self._make_state())

                elif action == "hello":
                    await websocket.send(self._make_hello())
                    # Resync
                    for entry in self.scan_buf:
                        await websocket.send(encode_dp_frame(**entry))
                    await websocket.send(self._make_resync_complete())

                elif action == "zero":
                    await websocket.send(json.dumps({"t": "event", "name": "zero_done"}))

        except Exception as exc:
            log.debug("Client disconnected: %s", exc)
        finally:
            self.clients.discard(websocket)
            log.info("Client disconnected (%d remain)", len(self.clients))


_server_instance = MockServer()


async def start_server(host: str = "localhost", port: int = 8765):
    """Start the mock WS server and return (server_object, port)."""
    try:
        import websockets
    except ImportError:
        raise ImportError("pip install websockets>=12")

    srv = await websockets.serve(_server_instance.handle_client, host, port)
    actual_port = list(srv.sockets)[0].getsockname()[1]
    log.info("Mock WS server running at ws://%s:%d", host, actual_port)
    return srv, actual_port


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Aqua-HMET mock WS server")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--host", type=str, default="localhost")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    async def main():
        srv, port = await start_server(args.host, args.port)
        print(f"Mock server: ws://{args.host}:{port}")
        print("Ctrl+C to stop")
        await srv.wait_closed()

    asyncio.run(main())
