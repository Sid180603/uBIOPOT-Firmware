"""
test_p5_protocol.py — Pytest L5 protocol conformance tests for P5 net_comms.

Layers tested:
  Section A: Pure frame parsing           (no server, no network — runs in CI)
  Section B: Mock WS server interaction   (in-process asyncio — runs in CI)
  Section C: Live device integration      (skipped unless AQUAHMET_HOST is set)

Run all CI-safe tests:
  pytest test/test_p5_protocol.py -v -k "not device"

Run against real device at 192.168.4.1:
  AQUAHMET_HOST=192.168.4.1 pytest test/test_p5_protocol.py -v

Dependencies:
  pip install -r test/requirements.txt
"""

import asyncio
import json
import os
import struct
import sys
import time
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Import the mock server module (same directory)
# ---------------------------------------------------------------------------

sys.path.insert(0, str(Path(__file__).parent))
from mock_ws_server import (
    WS_FRAME_TYPE_DP, WS_FRAME_SIZE, WS_FRAME_FMT, CSV_HEADER,
    EVENT_STARTED, EVENT_COMPLETE, EVENT_ABORTED,
    encode_dp_frame, decode_dp_frame, synthetic_current,
    start_server, _server_instance,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

DEVICE_HOST = os.environ.get("AQUAHMET_HOST", "")
DEVICE_SKIP = pytest.mark.skipif(
    not DEVICE_HOST,
    reason="AQUAHMET_HOST not set — skipping live device tests"
)


def parse_dp_frame(data: bytes) -> dict:
    """Parse a 16-byte DataPoint binary frame. Raises on bad data."""
    assert len(data) == WS_FRAME_SIZE, f"Frame must be {WS_FRAME_SIZE} bytes, got {len(data)}"
    ft, elec, idx, E, I, RE = struct.unpack(WS_FRAME_FMT, data)
    assert ft == WS_FRAME_TYPE_DP, f"frame_type must be 0x{WS_FRAME_TYPE_DP:02x}, got 0x{ft:02x}"
    return {"frame_type": ft, "electrode": elec, "idx": idx,
            "E_mV": E, "I_uA": I, "RE_mV": RE}


# ===========================================================================
# SECTION A — Pure frame encoding / decoding (no server needed)
# ===========================================================================

class TestWsFrameEncoding:
    """All tests are pure struct/math — zero network, always pass in CI."""

    def test_frame_size_is_16(self):
        data = encode_dp_frame(1, 0, 0.0, 0.0, 0.0)
        assert len(data) == 16

    def test_frame_type_byte_is_0x01(self):
        data = encode_dp_frame(1, 0, 0.0, 0.0, 0.0)
        assert data[0] == 0x01

    def test_electrode_byte_at_offset_1(self):
        data = encode_dp_frame(electrode=3, idx=0, E_mV=0.0, I_uA=0.0, RE_mV=0.0)
        assert data[1] == 3

    def test_idx_little_endian_at_offset_2(self):
        data = encode_dp_frame(1, 0x0102, 0.0, 0.0, 0.0)
        # LE: low byte first
        assert data[2] == 0x02
        assert data[3] == 0x01

    def test_E_mV_float_at_offset_4(self):
        E = -500.0
        data = encode_dp_frame(1, 0, E, 0.0, 0.0)
        E_decoded, = struct.unpack_from("<f", data, 4)
        assert abs(E_decoded - E) < 1e-3

    def test_I_uA_float_at_offset_8(self):
        I = 12.3456
        data = encode_dp_frame(1, 0, 0.0, I, 0.0)
        I_decoded, = struct.unpack_from("<f", data, 8)
        assert abs(I_decoded - I) < 1e-3

    def test_RE_mV_float_at_offset_12(self):
        RE = -495.5
        data = encode_dp_frame(1, 0, 0.0, 0.0, RE)
        RE_decoded, = struct.unpack_from("<f", data, 12)
        assert abs(RE_decoded - RE) < 1e-3

    def test_roundtrip_known_values(self):
        orig = dict(electrode=2, idx=42, E_mV=-250.0, I_uA=8.5, RE_mV=-248.0)
        data = encode_dp_frame(**orig)
        decoded = decode_dp_frame(data)
        assert decoded is not None
        assert decoded["electrode"] == 2
        assert decoded["idx"] == 42
        assert abs(decoded["E_mV"]  - (-250.0)) < 1e-3
        assert abs(decoded["I_uA"]  - 8.5)      < 1e-3
        assert abs(decoded["RE_mV"] - (-248.0)) < 1e-3

    def test_decode_returns_none_for_wrong_size(self):
        assert decode_dp_frame(b"\x01" * 15) is None   # too short
        assert decode_dp_frame(b"\x01" * 17) is None   # too long

    def test_decode_returns_none_for_wrong_frame_type(self):
        data = bytearray(encode_dp_frame(1, 0, 0.0, 0.0, 0.0))
        data[0] = 0x02   # wrong type
        assert decode_dp_frame(bytes(data)) is None

    def test_all_three_electrodes_encode(self):
        for elec in (1, 2, 3):
            data = encode_dp_frame(elec, 0, 0.0, 0.0, 0.0)
            decoded = decode_dp_frame(data)
            assert decoded["electrode"] == elec

    def test_max_idx_value(self):
        data = encode_dp_frame(1, 0xFFFF, 0.0, 0.0, 0.0)
        decoded = decode_dp_frame(data)
        assert decoded["idx"] == 0xFFFF


class TestCsvFormat:
    """CSV wire format contract."""

    def test_header_contains_all_columns(self):
        for col in ("electrode", "idx", "E_mV", "I_uA", "RE_mV"):
            assert col in CSV_HEADER, f"Missing column '{col}' in CSV header"

    def test_header_ends_with_crlf(self):
        assert CSV_HEADER.endswith("\r\n"), "CSV header must end with CRLF (RFC 4180)"

    def test_header_comma_separated(self):
        cols = CSV_HEADER.strip().split(",")
        assert len(cols) == 5

    def test_row_format_correct_values(self):
        from mock_ws_server import CSV_ROW_FMT
        row = CSV_ROW_FMT.format(electrode=1, idx=5,
                                 E_mV=-500.0, I_uA=12.3456, RE_mV=-495.0)
        assert "1,5,-500.0000,12.3456,-495.0000" in row
        assert row.endswith("\r\n")

    def test_row_has_five_columns(self):
        from mock_ws_server import CSV_ROW_FMT
        row = CSV_ROW_FMT.format(electrode=1, idx=0,
                                 E_mV=0.0, I_uA=0.0, RE_mV=0.0)
        cols = row.strip().split(",")
        assert len(cols) == 5


class TestSyntheticCellModel:
    """Verify the cell simulator used in mock server + Wokwi chips."""

    def test_current_is_float(self):
        assert isinstance(synthetic_current(0.0), float)

    def test_cd_peak_near_minus_700mV(self):
        """Cd²⁺ peak centred at −700 mV — current there should exceed baseline."""
        I_peak   = synthetic_current(-700.0)
        I_before = synthetic_current(-900.0)
        assert I_peak > I_before

    def test_pb_peak_near_minus_400mV(self):
        I_peak   = synthetic_current(-400.0)
        I_flanks = (synthetic_current(-600.0) + synthetic_current(-200.0)) / 2
        assert I_peak > I_flanks

    def test_cu_peak_near_0mV(self):
        I_peak = synthetic_current(0.0)
        I_far  = synthetic_current(500.0)
        assert I_peak > I_far

    def test_current_is_positive_at_peaks(self):
        for E in (-700.0, -400.0, 0.0):
            assert synthetic_current(E) > 0.0


class TestJsonEventSchema:
    """JSON event and command schema — pure string/dict tests."""

    def _make_event(self, name, **kw):
        msg = {"t": "event", "name": name}
        msg.update(kw)
        return msg

    def _make_hello(self):
        return {"t": "hello", "fw": "1.0.0", "proto": 1}

    def _make_state(self, state="IDLE", pts=0):
        return {"t": "state", "state": state, "pts": pts}

    def test_hello_has_t_field(self):
        assert self._make_hello()["t"] == "hello"

    def test_hello_has_proto_field(self):
        assert "proto" in self._make_hello()

    def test_hello_proto_is_integer(self):
        assert isinstance(self._make_hello()["proto"], int)

    def test_hello_proto_is_1(self):
        assert self._make_hello()["proto"] == 1

    def test_event_scan_started_fields(self):
        msg = self._make_event("scan_started", mode="DPV", electrode=1)
        assert msg["t"] == "event"
        assert msg["name"] == "scan_started"
        assert "mode" in msg
        assert "electrode" in msg

    def test_event_scan_complete_fields(self):
        msg = self._make_event("scan_complete")
        assert msg["name"] == "scan_complete"

    def test_event_scan_error_has_msg(self):
        msg = self._make_event("scan_error", msg="Voltage out of range")
        assert "msg" in msg

    def test_state_fields(self):
        msg = self._make_state("RUNNING", 42)
        assert msg["t"] == "state"
        assert msg["state"] == "RUNNING"
        assert msg["pts"] == 42

    def test_state_valid_states(self):
        valid = {"IDLE", "EQUILIBRATING", "RUNNING", "COMPLETE", "ABORTING", "ERROR"}
        for s in valid:
            msg = self._make_state(s)
            assert msg["state"] in valid

    def test_json_serialisable(self):
        hello = self._make_hello()
        encoded = json.dumps(hello)
        decoded = json.loads(encoded)
        assert decoded["proto"] == 1

    def test_command_start_schema(self):
        cmd = {
            "cmd": "start",
            "technique": "DPV",
            "electrode": 1,
            "params": {
                "e_begin_mV": -500,
                "e_end_mV":    500,
                "e_step_mV":     5,
                "e_pulse_mV":   25,
                "t_pulse_ms":   50,
                "t_period_ms": 200,
            }
        }
        assert cmd["cmd"] == "start"
        assert "params" in cmd
        assert isinstance(cmd["params"], dict)
        encoded = json.dumps(cmd)
        assert "start" in encoded

    def test_command_abort_schema(self):
        cmd = {"cmd": "abort"}
        assert json.loads(json.dumps(cmd))["cmd"] == "abort"

    def test_command_state_schema(self):
        cmd = {"cmd": "state"}
        assert json.loads(json.dumps(cmd))["cmd"] == "state"


# ===========================================================================
# SECTION B — Mock WS server interaction (in-process asyncio, CI-safe)
# ===========================================================================

@pytest.mark.asyncio
class TestMockWsServer:
    """Tests that spin up the in-process mock server and connect a WS client."""

    @pytest.fixture(autouse=True)
    async def server(self):
        """Start mock server on a random port, yield, then close."""
        import websockets
        srv, port = await start_server("127.0.0.1", 0)
        self.port = port
        yield
        srv.close()
        await srv.wait_closed()
        _server_instance.clients.clear()
        _server_instance.scan_buf.clear()
        _server_instance.state = "IDLE"

    async def _connect(self):
        import websockets
        return await websockets.connect(f"ws://127.0.0.1:{self.port}")

    async def test_hello_message_on_connect(self):
        ws = await self._connect()
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
            hello = json.loads(msg)
            assert hello["t"] == "hello"
            assert hello["proto"] == 1
            assert "fw" in hello
        finally:
            await ws.close()

    async def test_state_command_returns_state(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete (empty scan)
            await ws.send(json.dumps({"cmd": "state"}))
            resp = await asyncio.wait_for(ws.recv(), timeout=2.0)
            state = json.loads(resp)
            assert state["t"] == "state"
            assert "state" in state
            assert "pts" in state
        finally:
            await ws.close()

    async def test_hello_command_triggers_resync(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            await ws.send(json.dumps({"cmd": "hello"}))
            resp_hello = await asyncio.wait_for(ws.recv(), timeout=2.0)
            resp_resync = await asyncio.wait_for(ws.recv(), timeout=2.0)
            hello = json.loads(resp_hello)
            resync = json.loads(resp_resync)
            assert hello["t"] == "hello"
            assert resync["t"] == "resync_complete"
        finally:
            await ws.close()

    async def test_start_command_triggers_scan_started_event(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            cmd = {
                "cmd": "start", "electrode": 1,
                "params": {"e_begin_mV": -200, "e_end_mV": 0, "e_step_mV": 50},
            }
            await ws.send(json.dumps(cmd))
            # First non-binary message should be scan_started event
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
                if isinstance(msg, str):
                    evt = json.loads(msg)
                    if evt.get("t") == "event":
                        assert evt["name"] == EVENT_STARTED
                        break
        finally:
            await ws.close()

    async def test_scan_emits_binary_datapoint_frames(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            cmd = {
                "cmd": "start", "electrode": 2,
                "params": {"e_begin_mV": 0, "e_end_mV": 30, "e_step_mV": 10},
            }
            await ws.send(json.dumps(cmd))

            binary_frames = []
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    break
                if isinstance(msg, bytes):
                    binary_frames.append(msg)
                elif isinstance(msg, str):
                    evt = json.loads(msg)
                    if evt.get("name") == EVENT_COMPLETE:
                        break

            assert len(binary_frames) >= 1, "Expected at least one binary DataPoint frame"

            # Validate each frame
            for frame_data in binary_frames:
                dp = decode_dp_frame(frame_data)
                assert dp is not None, "Failed to decode DataPoint frame"
                assert dp["frame_type"] == WS_FRAME_TYPE_DP
                assert dp["electrode"] == 2
                assert isinstance(dp["E_mV"], float)
                assert isinstance(dp["I_uA"], float)
        finally:
            await ws.close()

    async def test_datapoint_frames_have_sequential_idx(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            cmd = {
                "cmd": "start", "electrode": 1,
                "params": {"e_begin_mV": -100, "e_end_mV": 100, "e_step_mV": 50},
            }
            await ws.send(json.dumps(cmd))

            indices = []
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    break
                if isinstance(msg, bytes):
                    dp = decode_dp_frame(msg)
                    if dp:
                        indices.append(dp["idx"])
                elif isinstance(msg, str):
                    if json.loads(msg).get("name") == EVENT_COMPLETE:
                        break

            assert indices == list(range(len(indices))), \
                f"Indices not sequential: {indices}"

        finally:
            await ws.close()

    async def test_scan_complete_event_emitted(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            cmd = {
                "cmd": "start", "electrode": 1,
                "params": {"e_begin_mV": 0, "e_end_mV": 20, "e_step_mV": 10},
            }
            await ws.send(json.dumps(cmd))

            got_complete = False
            deadline = time.monotonic() + 6.0
            while time.monotonic() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    break
                if isinstance(msg, str):
                    evt = json.loads(msg)
                    if evt.get("name") == EVENT_COMPLETE:
                        got_complete = True
                        break

            assert got_complete, "scan_complete event never received"
        finally:
            await ws.close()

    async def test_abort_stops_scan(self):
        ws = await self._connect()
        try:
            await ws.recv()   # hello
            await ws.recv()   # resync_complete
            # Start a long scan
            cmd = {
                "cmd": "start", "electrode": 1,
                "params": {"e_begin_mV": -900, "e_end_mV": 900, "e_step_mV": 5},
            }
            await ws.send(json.dumps(cmd))

            # Wait for first binary frame (scan has started)
            got_frame = False
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                if isinstance(msg, bytes):
                    got_frame = True
                    break

            assert got_frame, "No binary frames received before abort"

            # Abort
            await ws.send(json.dumps({"cmd": "abort"}))

            # Expect scan_aborted event
            got_aborted = False
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    break
                if isinstance(msg, str):
                    evt = json.loads(msg)
                    if evt.get("name") == EVENT_ABORTED:
                        got_aborted = True
                        break

            assert got_aborted, "scan_aborted event never received after abort"
        finally:
            await ws.close()

    async def test_two_clients_both_receive_frames(self):
        ws1 = await self._connect()
        ws2 = await self._connect()
        try:
            # Drain hello/resync from both
            for ws in (ws1, ws2):
                await ws.recv()
                await ws.recv()

            # Start scan from ws1
            cmd = {
                "cmd": "start", "electrode": 1,
                "params": {"e_begin_mV": 0, "e_end_mV": 20, "e_step_mV": 10},
            }
            await ws1.send(json.dumps(cmd))

            # Both clients should receive binary frames
            frames1, frames2 = [], []
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                done = False
                for ws, lst in ((ws1, frames1), (ws2, frames2)):
                    try:
                        msg = await asyncio.wait_for(ws.recv(), timeout=0.1)
                        if isinstance(msg, bytes):
                            lst.append(msg)
                        elif isinstance(msg, str):
                            if json.loads(msg).get("name") == EVENT_COMPLETE:
                                done = True
                    except asyncio.TimeoutError:
                        pass
                if done and frames1 and frames2:
                    break

            assert len(frames1) > 0, "ws1 received no binary frames"
            assert len(frames2) > 0, "ws2 received no binary frames"
        finally:
            await ws1.close()
            await ws2.close()


# ===========================================================================
# SECTION C — Live device integration (requires AQUAHMET_HOST env var)
# ===========================================================================

@DEVICE_SKIP
@pytest.mark.asyncio
class TestDeviceIntegration:
    """
    Integration tests against a real Aqua-HMET device.
    Run with: AQUAHMET_HOST=192.168.4.1 pytest test/test_p5_protocol.py -v
    """

    @property
    def base_url(self):
        return f"http://{DEVICE_HOST}"

    @property
    def ws_url(self):
        return f"ws://{DEVICE_HOST}/ws"

    async def test_api_state_returns_json(self):
        import urllib.request
        with urllib.request.urlopen(f"{self.base_url}/api/state", timeout=5) as resp:
            body = resp.read()
            data = json.loads(body)
            assert "state" in data

    async def test_api_scan_csv_returns_csv_header(self):
        import urllib.request
        with urllib.request.urlopen(f"{self.base_url}/api/scan.csv", timeout=5) as resp:
            first_line = resp.readline().decode()
            assert "electrode" in first_line
            assert "E_mV" in first_line

    async def test_ws_hello_on_connect(self):
        import websockets
        async with websockets.connect(self.ws_url, open_timeout=5) as ws:
            msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
            hello = json.loads(msg)
            assert hello["t"] == "hello"
            assert hello["proto"] == 1

    async def test_ws_state_command(self):
        import websockets
        async with websockets.connect(self.ws_url, open_timeout=5) as ws:
            await ws.recv()    # hello
            # Drain until resync_complete
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
                if isinstance(msg, str) and json.loads(msg).get("t") == "resync_complete":
                    break
                # Skip binary frames (resync data points)

            await ws.send(json.dumps({"cmd": "state"}))
            resp = await asyncio.wait_for(ws.recv(), timeout=5.0)
            state = json.loads(resp)
            assert state["t"] == "state"

    async def test_root_serves_html(self):
        import urllib.request
        with urllib.request.urlopen(f"{self.base_url}/", timeout=5) as resp:
            ct = resp.headers.get("Content-Type", "")
            assert "text/html" in ct


# ===========================================================================
# conftest: register asyncio mode
# ===========================================================================

# pytest-asyncio >= 0.23 needs asyncio_mode in pytest.ini or as marker.
# For convenience, set it here if pytest-asyncio supports it.
def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "asyncio: mark test as async (pytest-asyncio)"
    )
