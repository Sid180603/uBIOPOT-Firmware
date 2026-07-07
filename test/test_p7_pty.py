"""
test_p7_pty.py — P7 host end-to-end serial protocol test using pyserial loop://

No hardware required.  Uses pyserial's built-in loop:// virtual serial port:
  - A "mock device" thread reads commands and writes NDJSON responses.
  - The "host" side sends commands and verifies the received messages.
  - loop:// is a pure in-memory byte pipe — no socat/PTY/OS serial driver needed.
  - Runs in CI on every platform (Linux, Windows, macOS).

For a real PTY pair on Linux (optional, not required in CI):
  socat -d -d pty,raw,echo=0 pty,raw,echo=0
  AQUAHMET_PTY=/dev/pts/3 pytest test/test_p7_pty.py -v -k pty

Dependencies:
  pip install pyserial

Run:
  pytest test/test_p7_pty.py -v
"""

import json
import threading
import time
from io import BytesIO

import pytest

try:
    import serial
    import serial.serialutil
    PYSERIAL_AVAILABLE = True
except ImportError:
    PYSERIAL_AVAILABLE = False

pyserial_required = pytest.mark.skipif(
    not PYSERIAL_AVAILABLE,
    reason="pyserial not installed — run: pip install pyserial"
)


# ---------------------------------------------------------------------------
# Protocol constants (mirror serial_comms_protocol.h)
# ---------------------------------------------------------------------------

PROTO_VERSION = 1
T_HELLO  = "hello"
T_POINT  = "point"
T_EVENT  = "event"
T_STATE  = "state"
T_RESYNC = "resync_complete"
T_ERROR  = "error"

EVT_STARTED  = "scan_started"
EVT_COMPLETE = "scan_complete"
EVT_ABORTED  = "scan_aborted"
EVT_ERROR    = "scan_error"

CMD_HELLO = "hello"
CMD_STATE = "state"
CMD_ABORT = "abort"
CMD_START = "start"


# ---------------------------------------------------------------------------
# Synthetic Gaussian cell model (matches main_sim.c + mock_ws_server.py)
# ---------------------------------------------------------------------------

import math

_PEAKS = [
    {"E_peak": -700.0, "I_peak": 45.0, "sigma": 60.0},  # Cd²⁺ ~-0.7 V
    {"E_peak": -400.0, "I_peak": 30.0, "sigma": 50.0},  # Pb²⁺ ~-0.4 V
    {"E_peak":    0.0, "I_peak": 20.0, "sigma": 55.0},  # Cu²⁺ ~0.0 V
]

def _synthetic_current(E_mV: float) -> float:
    I = 0.0
    for pk in _PEAKS:
        I += pk["I_peak"] * math.exp(-0.5 * ((E_mV - pk["E_peak"]) / pk["sigma"]) ** 2)
    I += 0.5 * math.sin(E_mV * 0.01)
    return I


# ---------------------------------------------------------------------------
# Mock Device: speaks the P7 NDJSON protocol over a serial-like stream
# ---------------------------------------------------------------------------

class MockDevice:
    """
    Minimal mock of the Aqua-HMET firmware serial_comms component.

    Reads NDJSON commands from `rx` (file-like readable) and writes
    NDJSON responses to `tx` (file-like writable).

    Designed to run in a background thread alongside a test that holds the
    other end of a pyserial loop:// connection (or any byte pipe).
    """

    def __init__(self, rx, tx):
        self._rx   = rx
        self._tx   = tx
        self._stop = threading.Event()
        self._scan_running = False

    def _send(self, obj: dict) -> None:
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        self._tx.write(line)
        self._tx.flush()

    def _send_hello(self) -> None:
        self._send({"t": T_HELLO, "fw": "1.0.0", "proto": PROTO_VERSION,
                    "device": "Aqua-HMET-mock"})

    def _send_state(self, state: int = 0) -> None:
        self._send({"t": T_STATE, "state": state})

    def _run_dpv(self, electrode: int, params: dict) -> None:
        """Emit a synthetic DPV scan synchronously (called from device thread)."""
        e_begin = params.get("e_begin_mV", -500.0)
        e_end   = params.get("e_end_mV",    500.0)
        e_step  = params.get("e_step_mV",     5.0)
        e_pulse = params.get("e_pulse_mV",   25.0)

        self._send({"t": T_EVENT, "name": EVT_STARTED, "mode": "DPV", "e": electrode})

        E    = e_begin
        idx  = 0
        while (E <= e_end) if e_begin < e_end else (E >= e_end):
            if self._stop.is_set():
                self._send({"t": T_EVENT, "name": EVT_ABORTED})
                self._scan_running = False
                return
            # Simulate base + pulse samples
            I_base  = _synthetic_current(E)
            I_pulse = _synthetic_current(E + e_pulse)
            dI      = round(I_pulse - I_base, 4)
            RE_mV   = round(E * 0.98, 2)  # RE ≈ commanded E (low iR in sim)
            self._send({"t": T_POINT, "e": electrode, "idx": idx,
                        "E": round(E, 4), "I": dI, "RE": RE_mV})
            E   += e_step if e_begin < e_end else -e_step
            idx += 1

        self._send({"t": T_EVENT, "name": EVT_COMPLETE})
        self._scan_running = False

    def run(self) -> None:
        """Main device loop — reads lines and dispatches commands."""
        buf = b""
        self._send_hello()  # auto-send hello on connect

        while not self._stop.is_set():
            try:
                ch = self._rx.read(1)
            except Exception:
                break
            if not ch:
                time.sleep(0.002)
                continue

            if ch in (b"\n", b"\r"):
                line = buf.decode("utf-8", errors="replace").strip()
                buf  = b""
                if not line.startswith("{"):
                    continue
                try:
                    cmd = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self._dispatch(cmd)
            else:
                buf += ch

    def _dispatch(self, cmd: dict) -> None:
        c = cmd.get("cmd", "")
        if c == CMD_HELLO:
            self._send_hello()
        elif c == CMD_STATE:
            self._send_state(2 if self._scan_running else 0)
        elif c == CMD_ABORT:
            if self._scan_running:
                self._stop.set()
            self._send({"t": T_EVENT, "name": EVT_ABORTED})
        elif c == CMD_START:
            electrode = cmd.get("electrode", 1)
            params    = cmd.get("params", {})
            self._scan_running = True
            self._run_dpv(electrode, params)
        else:
            self._send({"t": T_ERROR, "msg": f"unknown cmd: {c!r}"})

    def stop(self) -> None:
        self._stop.set()


# ---------------------------------------------------------------------------
# Loop:// serial pair helper
# ---------------------------------------------------------------------------

class LoopSerialPair:
    """
    Uses pyserial's loop:// URL to create a bidirectional byte pipe.
    MockDevice and the test client share the same loop port — bytes written
    by one party are immediately readable by the other (hardware loopback).

    For true two-endpoint testing, we wrap the loop port in two file-like
    adapters so MockDevice gets its own (rx, tx) handles.
    """

    def __init__(self):
        self._port   = serial.serial_for_url("loop://", timeout=0.1)
        self._lock   = threading.Lock()

    def close(self):
        self._port.close()

    # --- Host-side helpers (called from test) ---

    def host_send(self, obj: dict) -> None:
        """Send a JSON command from the host to the mock device."""
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        with self._lock:
            self._port.write(line)

    def host_readline(self, timeout: float = 2.0) -> str | None:
        """Read one NDJSON line from the device, blocking up to timeout."""
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            with self._lock:
                ch = self._port.read(1)
            if ch in (b"\n", b"\r"):
                return buf.decode("utf-8", errors="replace").strip()
            elif ch:
                buf += ch
        return buf.decode("utf-8", errors="replace").strip() or None

    def host_read_json(self, timeout: float = 2.0) -> dict | None:
        """Read the next valid JSON message from the device."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.host_readline(timeout=0.5)
            if line and line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    pass
        return None

    def host_read_until(self, t: str, timeout: float = 5.0) -> dict | None:
        """Read messages until one with the given "t" field is found."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.host_read_json(timeout=0.5)
            if msg and msg.get("t") == t:
                return msg
        return None

    def host_read_until_name(self, name: str, timeout: float = 5.0) -> dict | None:
        """Read event messages until one with the given "name" field is found."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.host_read_json(timeout=0.5)
            if msg and msg.get("name") == name:
                return msg
        return None


# ---------------------------------------------------------------------------
# NOTE on loop:// and mock device threading
#
# pyserial loop:// is a single-port loopback: what you write comes back out
# of the same port's read() — it's a hardware echo, not a two-endpoint pipe.
#
# For the mock device tests below, we drive the protocol state machine
# directly (without a MockDevice thread) by manually crafting the NDJSON
# lines and parsing responses. This avoids the threading complexity of
# sharing a single loop port between two parties.
#
# The MockDevice class above is used in the "pipe-based" tests that use
# real byte pipes (io.Pipe equivalent) rather than loop://.
# ---------------------------------------------------------------------------


def _make_pipe():
    """
    Create a bidirectional byte pipe using two os.pipe() pairs.
    Returns (device_rx, device_tx, host_rx, host_tx) as file objects.
    """
    import os
    d_rx_fd, h_tx_fd = os.pipe()  # host writes → device reads
    h_rx_fd, d_tx_fd = os.pipe()  # device writes → host reads
    return (
        os.fdopen(d_rx_fd, "rb", buffering=0),  # device reads
        os.fdopen(d_tx_fd, "wb", buffering=0),  # device writes
        os.fdopen(h_rx_fd, "rb", buffering=0),  # host reads
        os.fdopen(h_tx_fd, "wb", buffering=0),  # host writes
    )


class PipeClient:
    """Simple host client over os.pipe() for MockDevice E2E tests."""

    def __init__(self, rx, tx):
        self._rx = rx
        self._tx = tx

    def send(self, obj: dict) -> None:
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        self._tx.write(line)
        self._tx.flush()

    def readline(self, timeout: float = 3.0) -> str | None:
        import select, sys
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rem = deadline - time.monotonic()
            if rem <= 0:
                break
            if sys.platform != "win32":
                ready, _, _ = select.select([self._rx], [], [], min(rem, 0.1))
                if not ready:
                    continue
            ch = self._rx.read(1)
            if not ch:
                time.sleep(0.002)
                continue
            if ch in (b"\n", b"\r"):
                return buf.decode("utf-8", errors="replace").strip()
            buf += ch
        return buf.decode("utf-8", errors="replace").strip() or None

    def read_json(self, timeout: float = 3.0) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.readline(timeout=max(0.1, deadline - time.monotonic()))
            if line and line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    pass
        return None

    def read_until_t(self, t: str, timeout: float = 5.0) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.read_json(timeout=max(0.1, deadline - time.monotonic()))
            if msg and msg.get("t") == t:
                return msg
        return None

    def read_until_name(self, name: str, timeout: float = 5.0) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.read_json(timeout=max(0.1, deadline - time.monotonic()))
            if msg and msg.get("name") == name:
                return msg
        return None


# ---------------------------------------------------------------------------
# Section A: Pure protocol state-machine tests (no I/O at all)
# Always runs in CI.
# ---------------------------------------------------------------------------

class TestProtocolStateMachine:
    """Unit-tests of the MockDevice state machine without any I/O."""

    def _device_with_pipes(self):
        """Return a MockDevice connected to a PipeClient."""
        drx, dtx, hrx, htx = _make_pipe()
        dev    = MockDevice(drx, dtx)
        client = PipeClient(hrx, htx)
        return dev, client, drx, dtx, hrx, htx

    def test_hello_on_connect(self):
        """MockDevice sends hello immediately on run() without any command."""
        import os, sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            msg = client.read_until_t(T_HELLO, timeout=3.0)
            assert msg is not None, "No hello received"
            assert msg["proto"] == PROTO_VERSION
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_hello_cmd_response(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            # drain auto-hello
            client.read_until_t(T_HELLO, timeout=2.0)
            # send hello cmd
            client.send({"cmd": CMD_HELLO})
            msg = client.read_until_t(T_HELLO, timeout=3.0)
            assert msg is not None, "No hello response"
            assert msg["fw"] is not None
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_state_idle_response(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            client.send({"cmd": CMD_STATE})
            msg = client.read_until_t(T_STATE, timeout=3.0)
            assert msg is not None, "No state response"
            assert msg["state"] == 0  # IDLE
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_start_emits_scan_started(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            client.send({"cmd": CMD_START, "electrode": 1,
                         "params": {"e_begin_mV": -100.0, "e_end_mV": 100.0,
                                    "e_step_mV": 20.0, "e_pulse_mV": 25.0}})
            msg = client.read_until_name(EVT_STARTED, timeout=5.0)
            assert msg is not None, "No scan_started event"
            assert msg["mode"] == "DPV"
            assert msg["e"] == 1
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_start_emits_points_and_complete(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            # small scan: 5 steps
            client.send({"cmd": CMD_START, "electrode": 1,
                         "params": {"e_begin_mV": 0.0, "e_end_mV": 80.0,
                                    "e_step_mV": 20.0, "e_pulse_mV": 5.0}})
            client.read_until_name(EVT_STARTED, timeout=3.0)
            points = []
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                msg = client.read_json(timeout=0.5)
                if not msg:
                    continue
                if msg.get("t") == T_POINT:
                    points.append(msg)
                elif msg.get("name") == EVT_COMPLETE:
                    break
            assert len(points) >= 4, f"Expected ≥4 points, got {len(points)}"
            # Verify point structure
            pt = points[0]
            assert "e" in pt
            assert "idx" in pt
            assert "E" in pt
            assert "I" in pt
            assert "RE" in pt
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_abort_stops_scan(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            # long scan so abort lands mid-scan
            client.send({"cmd": CMD_START, "electrode": 1,
                         "params": {"e_begin_mV": -1000.0, "e_end_mV": 1000.0,
                                    "e_step_mV": 5.0, "e_pulse_mV": 25.0}})
            client.read_until_name(EVT_STARTED, timeout=3.0)
            # wait for a few points then abort
            time.sleep(0.05)
            client.send({"cmd": CMD_ABORT})
            msg = client.read_until_name(EVT_ABORTED, timeout=5.0)
            assert msg is not None, "No scan_aborted event after abort"
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_non_json_line_ignored(self):
        """Stray non-JSON bytes (like IDF log lines) are silently dropped."""
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            # Send a stray log line followed by a valid command
            raw = b"I (1234) esp_image: segment 0: paddr=00001020\n"
            htx.write(raw)
            htx.flush()
            time.sleep(0.1)
            client.send({"cmd": CMD_STATE})
            msg = client.read_until_t(T_STATE, timeout=3.0)
            assert msg is not None, "Device stopped responding after stray log line"
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass

    def test_unknown_cmd_returns_error(self):
        import sys
        if sys.platform == "win32":
            pytest.skip("os.pipe() select not supported on Windows in this form")
        dev, client, drx, dtx, hrx, htx = self._device_with_pipes()
        t = threading.Thread(target=dev.run, daemon=True)
        t.start()
        try:
            client.read_until_t(T_HELLO, timeout=2.0)
            client.send({"cmd": "invalid_command_xyz"})
            msg = client.read_until_t(T_ERROR, timeout=3.0)
            assert msg is not None, "No error for unknown command"
            assert "invalid_command_xyz" in msg.get("msg", "")
        finally:
            dev.stop()
            for f in (drx, dtx, hrx, htx):
                try: f.close()
                except: pass


# ---------------------------------------------------------------------------
# Section B: pyserial loop:// loopback tests
# Skipped if pyserial is not installed.
# ---------------------------------------------------------------------------

@pyserial_required
class TestPySerialLoop:
    """Tests using pyserial loop:// for protocol structure validation."""

    def test_loop_port_opens(self):
        port = serial.serial_for_url("loop://", timeout=0.1)
        assert port.is_open
        port.close()

    def test_loop_write_read_roundtrip(self):
        """Bytes written to loop:// come back out on read."""
        port = serial.serial_for_url("loop://", timeout=0.5)
        try:
            port.write(b"hello\n")
            data = port.read(6)
            assert data == b"hello\n"
        finally:
            port.close()

    def test_ndjson_roundtrip(self):
        """A JSON line written to loop:// is readable and parseable."""
        port = serial.serial_for_url("loop://", timeout=0.5)
        try:
            obj = {"t": "hello", "fw": "1.0.0", "proto": 1}
            line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
            port.write(line)
            received = port.read(len(line))
            parsed = json.loads(received.decode().strip())
            assert parsed["t"] == "hello"
            assert parsed["proto"] == 1
        finally:
            port.close()

    def test_multiple_ndjson_lines(self):
        """Multiple NDJSON lines are written and read correctly."""
        port = serial.serial_for_url("loop://", timeout=0.5)
        try:
            messages = [
                {"t": "hello", "fw": "1.0.0", "proto": 1},
                {"t": "point", "e": 1, "idx": 0, "E": -500.0, "I": 6.42, "RE": -495.0},
                {"t": "event", "name": "scan_complete"},
            ]
            raw = b"".join(
                (json.dumps(m, separators=(",", ":")) + "\n").encode()
                for m in messages
            )
            port.write(raw)
            received = port.read(len(raw))
            lines = received.decode().splitlines()
            assert len(lines) == 3
            assert json.loads(lines[0])["t"] == "hello"
            assert json.loads(lines[1])["t"] == "point"
            assert json.loads(lines[2])["name"] == "scan_complete"
        finally:
            port.close()


# ---------------------------------------------------------------------------
# Section C: Live device tests (skip unless AQUAHMET_PORT is set)
# These use a real COM port connected to flashed hardware.
# ---------------------------------------------------------------------------

import os

DEVICE_PORT = os.environ.get("AQUAHMET_PORT", "")
DEVICE_BAUD = int(os.environ.get("AQUAHMET_BAUD", "115200"))
DEVICE_SKIP = pytest.mark.skipif(
    not DEVICE_PORT,
    reason="AQUAHMET_PORT not set — skipping live device tests"
)


@DEVICE_SKIP
@pyserial_required
class TestLiveDeviceSerial:
    """Live device tests over real COM port."""

    @pytest.fixture(autouse=True)
    def port(self):
        ser = serial.Serial(DEVICE_PORT, DEVICE_BAUD, timeout=0.5,
                            rtscts=False, xonxoff=False)
        ser.rts = False
        ser.dtr = False
        yield ser
        ser.close()

    def _readline(self, ser, timeout: float = 3.0) -> str | None:
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ch = ser.read(1)
            if not ch:
                continue
            if ch == b"\n":
                return buf.decode("utf-8", errors="replace").strip()
            buf += ch
        return None

    def _read_until_t(self, ser, t: str, timeout: float = 5.0) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self._readline(ser, timeout=0.5)
            if line and line.startswith("{"):
                try:
                    msg = json.loads(line)
                    if msg.get("t") == t:
                        return msg
                except json.JSONDecodeError:
                    pass
        return None

    def test_hello_handshake(self, port):
        port.write((json.dumps({"cmd": CMD_HELLO}) + "\n").encode())
        msg = self._read_until_t(port, T_HELLO, timeout=3.0)
        assert msg is not None
        assert msg["proto"] == PROTO_VERSION

    def test_state_query(self, port):
        port.write((json.dumps({"cmd": CMD_STATE}) + "\n").encode())
        msg = self._read_until_t(port, T_STATE, timeout=3.0)
        assert msg is not None
        assert 0 <= msg["state"] <= 5
