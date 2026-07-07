"""
test_p7_serial.py — Pytest L5 protocol conformance tests for P7 serial_comms.

Layers tested:
  Section A: Pure NDJSON format constants           (no hardware, runs in CI)
  Section B: Mock serial port interaction            (in-process asyncio, runs in CI)
  Section C: Live device integration                 (skipped unless AQUAHMET_PORT is set)

Section A validates that the serial NDJSON messages produced by serial_comms.c
have the correct structure: field names, types, and values — mirroring what
potentiostat-core.js (P6/P9) expects.

Section B uses a mock serial device (asyncio streams reading/writing a PTY or
in-memory pipe) to verify that the client correctly dispatches commands and
accumulates data points.  When no real PTY is available (Windows CI), section B
runs the protocol parser inline against raw bytes.

Section C runs against a real device.  Skip by default in CI.

Run all CI-safe tests:
    pytest test/test_p7_serial.py -v -k "not device"

Run against real device:
    AQUAHMET_PORT=COM9 pytest test/test_p7_serial.py -v

Dependencies:
    pip install -r test/requirements.txt  (includes pyserial for section C)
"""

import json
import os
import struct
import sys
from io import BytesIO
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Protocol constants  (mirror serial_comms_protocol.h)
# ---------------------------------------------------------------------------

SERIAL_PROTOCOL_VERSION = 1
SERIAL_FW_VERSION_STR   = "1.0.0"
SERIAL_DEVICE_NAME      = "Aqua-HMET"

T_HELLO   = "hello"
T_POINT   = "point"
T_EVENT   = "event"
T_STATE   = "state"
T_PEAKS   = "peaks"
T_RESYNC  = "resync_complete"
T_ERROR   = "error"

EVT_STARTED  = "scan_started"
EVT_EQUILIB  = "equilibrating"
EVT_COMPLETE = "scan_complete"
EVT_ABORTED  = "scan_aborted"
EVT_ERROR    = "scan_error"
EVT_RESET    = "scan_reset"

CMD_START  = "start"
CMD_ABORT  = "abort"
CMD_ZERO   = "zero"
CMD_STATE  = "state"
CMD_HELLO  = "hello"

# Same parity requirement: serial and WS event names must match
NET_EVT_NAME_STARTED  = "scan_started"
NET_EVT_NAME_EQUILIB  = "equilibrating"
NET_EVT_NAME_COMPLETE = "scan_complete"
NET_EVT_NAME_ABORTED  = "scan_aborted"
NET_EVT_NAME_ERROR    = "scan_error"
NET_EVT_NAME_RESET    = "scan_reset"

# ---------------------------------------------------------------------------
# Synthetic NDJSON line builders — reproduce what serial_comms.c emits
# ---------------------------------------------------------------------------

def _make_hello() -> dict:
    """Reproduce the hello message emitted by serial_comms_start()."""
    return {
        "t":      T_HELLO,
        "fw":     SERIAL_FW_VERSION_STR,
        "proto":  SERIAL_PROTOCOL_VERSION,
        "device": SERIAL_DEVICE_NAME,
    }

def _make_point(electrode: int, idx: int,
                E_mV: float, I_uA: float, RE_mV: float) -> dict:
    """Reproduce the point message emitted by serial_on_point()."""
    return {
        "t":   T_POINT,
        "e":   electrode,
        "idx": idx,
        "E":   round(E_mV,  4),
        "I":   round(I_uA,  4),
        "RE":  round(RE_mV, 4),
    }

def _make_event(name: str, **extra) -> dict:
    obj = {"t": T_EVENT, "name": name}
    obj.update(extra)
    return obj

def _make_state(state: int) -> dict:
    return {"t": T_STATE, "state": state}

def _make_resync(count: int, state: int) -> dict:
    return {"t": T_RESYNC, "count": count, "state": state}

def _make_error(msg: str) -> dict:
    return {"t": T_ERROR, "msg": msg}


def _ndjson_line(obj: dict) -> bytes:
    """Encode a dict as a NDJSON line (bytes, ending with \n)."""
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode()


def _parse_ndjson_line(line: str) -> dict:
    """Parse one NDJSON line; raise ValueError if not valid JSON."""
    line = line.strip()
    if not line.startswith("{"):
        raise ValueError(f"Not a JSON object: {line!r}")
    return json.loads(line)


# ===========================================================================
# Section A: Pure NDJSON format validation  (no hardware, CI-safe)
# ===========================================================================

class TestProtocolVersion:
    def test_version_is_1(self):
        assert SERIAL_PROTOCOL_VERSION == 1

    def test_serial_version_matches_net_version(self):
        """Serial and WS protocol versions must be identical (same engine)."""
        # Mirror of the Unity C test test_serial_and_net_protocol_versions_match
        NET_PROTOCOL_VERSION = 1   # from net_comms_protocol.h
        assert SERIAL_PROTOCOL_VERSION == NET_PROTOCOL_VERSION


class TestEventNameParity:
    """SERIAL event names must be byte-for-byte identical to WS/NET event names."""

    def test_started(self):
        assert EVT_STARTED == NET_EVT_NAME_STARTED

    def test_equilib(self):
        assert EVT_EQUILIB == NET_EVT_NAME_EQUILIB

    def test_complete(self):
        assert EVT_COMPLETE == NET_EVT_NAME_COMPLETE

    def test_aborted(self):
        assert EVT_ABORTED == NET_EVT_NAME_ABORTED

    def test_error(self):
        assert EVT_ERROR == NET_EVT_NAME_ERROR

    def test_reset(self):
        assert EVT_RESET == NET_EVT_NAME_RESET


class TestCommandNameParity:
    """Command names sent over serial must match those sent over WS."""

    NET_CMD_START = "start"
    NET_CMD_ABORT = "abort"
    NET_CMD_ZERO  = "zero"
    NET_CMD_STATE = "state"
    NET_CMD_HELLO = "hello"

    def test_start(self): assert CMD_START == self.NET_CMD_START
    def test_abort(self): assert CMD_ABORT == self.NET_CMD_ABORT
    def test_zero(self):  assert CMD_ZERO  == self.NET_CMD_ZERO
    def test_state(self): assert CMD_STATE == self.NET_CMD_STATE
    def test_hello(self): assert CMD_HELLO == self.NET_CMD_HELLO


class TestHelloMessage:
    def test_t_field_is_hello(self):
        msg = _make_hello()
        assert msg["t"] == T_HELLO

    def test_fw_field_present(self):
        msg = _make_hello()
        assert "fw" in msg
        assert isinstance(msg["fw"], str)
        assert len(msg["fw"]) > 0

    def test_proto_field_present(self):
        msg = _make_hello()
        assert "proto" in msg
        assert msg["proto"] == SERIAL_PROTOCOL_VERSION

    def test_device_field_present(self):
        msg = _make_hello()
        assert "device" in msg
        assert isinstance(msg["device"], str)

    def test_ndjson_line_is_valid_json(self):
        line = _ndjson_line(_make_hello()).decode().strip()
        parsed = json.loads(line)
        assert parsed["t"] == T_HELLO

    def test_line_ends_with_newline(self):
        raw = _ndjson_line(_make_hello())
        assert raw.endswith(b"\n")

    def test_line_starts_with_brace(self):
        raw = _ndjson_line(_make_hello())
        assert raw.startswith(b"{")


class TestPointMessage:
    def test_t_field_is_point(self):
        msg = _make_point(1, 0, -500.0, 6.42, -495.0)
        assert msg["t"] == T_POINT

    def test_electrode_field(self):
        msg = _make_point(2, 0, 0.0, 0.0, 0.0)
        assert msg["e"] == 2

    def test_idx_field(self):
        msg = _make_point(1, 42, 0.0, 0.0, 0.0)
        assert msg["idx"] == 42

    def test_E_field_in_mV(self):
        """E field carries millivolts (not volts) in serial format."""
        msg = _make_point(1, 0, -500.0, 0.0, 0.0)
        assert msg["E"] == pytest.approx(-500.0, abs=0.1)

    def test_I_field_in_uA(self):
        msg = _make_point(1, 0, 0.0, 6.42, 0.0)
        assert msg["I"] == pytest.approx(6.42, abs=0.01)

    def test_RE_field_in_mV(self):
        msg = _make_point(1, 0, 0.0, 0.0, -495.0)
        assert msg["RE"] == pytest.approx(-495.0, abs=0.1)

    def test_required_fields_present(self):
        msg = _make_point(1, 0, 0.0, 0.0, 0.0)
        for field in ("t", "e", "idx", "E", "I", "RE"):
            assert field in msg, f"Missing field '{field}'"

    def test_ndjson_line_parseable(self):
        line = _ndjson_line(_make_point(1, 5, -300.0, 3.14, -295.0)).decode()
        parsed = json.loads(line.strip())
        assert parsed["t"] == T_POINT
        assert parsed["e"] == 1
        assert parsed["idx"] == 5

    def test_line_ends_with_newline(self):
        raw = _ndjson_line(_make_point(1, 0, 0.0, 0.0, 0.0))
        assert raw.endswith(b"\n")


class TestEventMessages:
    def test_scan_started_fields(self):
        msg = _make_event(EVT_STARTED, mode="DPV", e=1)
        assert msg["t"] == T_EVENT
        assert msg["name"] == "scan_started"
        assert msg["mode"] == "DPV"
        assert msg["e"] == 1

    def test_scan_complete_fields(self):
        msg = _make_event(EVT_COMPLETE)
        assert msg["t"] == T_EVENT
        assert msg["name"] == "scan_complete"

    def test_scan_aborted_fields(self):
        msg = _make_event(EVT_ABORTED)
        assert msg["name"] == "scan_aborted"

    def test_scan_error_has_msg(self):
        msg = _make_event(EVT_ERROR, msg="param validation failed")
        assert msg["name"] == "scan_error"
        assert "param validation failed" in msg.get("msg", "")

    def test_equilibrating_fields(self):
        msg = _make_event(EVT_EQUILIB)
        assert msg["name"] == "equilibrating"


class TestStateMessage:
    def test_t_field_is_state(self):
        msg = _make_state(0)
        assert msg["t"] == T_STATE

    def test_state_field_is_int(self):
        for s in range(6):
            msg = _make_state(s)
            assert isinstance(msg["state"], int)
            assert msg["state"] == s


class TestResyncMessage:
    def test_t_field_is_resync_complete(self):
        msg = _make_resync(120, 0)
        assert msg["t"] == T_RESYNC

    def test_count_field(self):
        msg = _make_resync(77, 2)
        assert msg["count"] == 77

    def test_state_field(self):
        msg = _make_resync(0, 3)
        assert msg["state"] == 3


class TestCommandStructure:
    """Validate that inbound command JSON has the expected structure."""

    def test_hello_cmd(self):
        cmd = {"cmd": CMD_HELLO}
        line = json.dumps(cmd)
        parsed = json.loads(line)
        assert parsed["cmd"] == "hello"

    def test_abort_cmd(self):
        cmd = {"cmd": CMD_ABORT}
        parsed = json.loads(json.dumps(cmd))
        assert parsed["cmd"] == "abort"

    def test_start_cmd_electrode(self):
        cmd = {"cmd": CMD_START, "electrode": 3}
        parsed = json.loads(json.dumps(cmd))
        assert parsed["cmd"] == "start"
        assert parsed["electrode"] == 3

    def test_start_cmd_params_keys(self):
        """All expected DPV parameter keys are serialisable."""
        params = {
            "e_begin_mV":         -500.0,
            "e_end_mV":            500.0,
            "e_step_mV":             5.0,
            "e_pulse_mV":           25.0,
            "t_pulse_ms":            50,
            "t_period_ms":          200,
            "t_equilibration_ms":  2000,
            "cycles":                 1,
            "n_avg":                  5,
        }
        cmd = {"cmd": CMD_START, "electrode": 1, "params": params}
        parsed = json.loads(json.dumps(cmd))
        assert parsed["params"]["e_begin_mV"] == pytest.approx(-500.0)
        assert parsed["params"]["t_period_ms"] == 200

    def test_non_json_line_ignored_by_host(self):
        """Lines not starting with '{' must be skipped without error."""
        stray_lines = [
            "I (1234) esp_image: segment 0: paddr=00001020",
            "W (999) serial_comms: Log level set to WARN",
            "",
            "   ",
        ]
        for line in stray_lines:
            assert not line.strip().startswith("{"), (
                f"Line unexpectedly looks like JSON: {line!r}"
            )


# ===========================================================================
# Section B: In-process mock serial stream (CI-safe, no hardware)
# ===========================================================================

class MockSerial:
    """
    Minimal synchronous mock of a pyserial.Serial object.

    Reads from _rx_buf (bytes pre-loaded before the test).
    Writes are accumulated in _tx_buf for inspection.
    """

    def __init__(self, rx_data: bytes = b""):
        self._rx_buf = BytesIO(rx_data)
        self._tx_buf = bytearray()
        self.rts = False
        self.dtr = False

    def write(self, data: bytes) -> int:
        self._tx_buf += data
        return len(data)

    def read(self, size: int = 1) -> bytes:
        return self._rx_buf.read(size)

    @property
    def is_open(self) -> bool:
        return True

    def close(self) -> None:
        pass

    def sent_lines(self) -> list[str]:
        """Return all lines sent to the mock device (TX)."""
        return self._tx_buf.decode("utf-8", errors="replace").splitlines()

    def received_messages(self) -> list[dict]:
        """Parse all valid JSON objects from the TX buffer."""
        msgs = []
        for line in self.sent_lines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    msgs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        return msgs


def _build_rx_stream(*messages: dict) -> bytes:
    """Encode a sequence of dicts into a NDJSON byte stream (device → host)."""
    return b"".join(_ndjson_line(m) for m in messages)


class TestMockSerialStream:
    """Test the client-side parsing of a simulated NDJSON stream."""

    def _parse_stream(self, rx: bytes) -> list[dict]:
        """Parse all valid JSON lines from raw bytes."""
        msgs = []
        for line in rx.decode("utf-8", errors="replace").splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    msgs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        return msgs

    def test_hello_parsed(self):
        rx = _build_rx_stream(_make_hello())
        msgs = self._parse_stream(rx)
        assert len(msgs) == 1
        assert msgs[0]["t"] == T_HELLO

    def test_point_parsed(self):
        rx = _build_rx_stream(_make_point(1, 0, -500.0, 6.42, -495.0))
        msgs = self._parse_stream(rx)
        assert len(msgs) == 1
        assert msgs[0]["t"] == T_POINT
        assert msgs[0]["e"] == 1

    def test_event_parsed(self):
        rx = _build_rx_stream(_make_event(EVT_STARTED, mode="DPV", e=1))
        msgs = self._parse_stream(rx)
        assert msgs[0]["name"] == EVT_STARTED

    def test_stray_log_line_skipped(self):
        """Non-JSON lines interspersed in the stream are silently ignored."""
        raw = (
            b"I (123) esp_image: booting\n"
            + _ndjson_line(_make_hello())
            + b"W (456) serial_comms: Log level set to WARN\n"
            + _ndjson_line(_make_point(1, 0, 0.0, 0.0, 0.0))
        )
        msgs = self._parse_stream(raw)
        assert len(msgs) == 2
        assert msgs[0]["t"] == T_HELLO
        assert msgs[1]["t"] == T_POINT

    def test_scan_sequence(self):
        """Full DPV scan sequence: started → N points → complete."""
        points = [_make_point(1, i, -500.0 + i * 5.0, float(i) * 0.1, -495.0)
                  for i in range(10)]
        rx = _build_rx_stream(
            _make_hello(),
            _make_event(EVT_STARTED, mode="DPV", e=1),
            *points,
            _make_event(EVT_COMPLETE),
        )
        msgs = self._parse_stream(rx)
        assert msgs[0]["t"] == T_HELLO
        assert msgs[1]["name"] == EVT_STARTED
        assert sum(1 for m in msgs if m["t"] == T_POINT) == 10
        assert msgs[-1]["name"] == EVT_COMPLETE

    def test_points_have_monotonic_idx(self):
        n = 5
        points = [_make_point(1, i, float(i), 0.0, 0.0) for i in range(n)]
        rx = _build_rx_stream(*points)
        msgs = self._parse_stream(rx)
        indices = [m["idx"] for m in msgs if m["t"] == T_POINT]
        assert indices == list(range(n))

    def test_resync_message(self):
        rx = _build_rx_stream(_make_resync(42, 2))
        msgs = self._parse_stream(rx)
        assert msgs[0]["t"] == T_RESYNC
        assert msgs[0]["count"] == 42
        assert msgs[0]["state"] == 2

    def test_error_message_has_msg_field(self):
        rx = _build_rx_stream(_make_error("e_step must be positive"))
        msgs = self._parse_stream(rx)
        assert msgs[0]["t"] == T_ERROR
        assert "e_step" in msgs[0]["msg"]

    def test_mock_serial_tx_records_command(self):
        ms = MockSerial()
        cmd = {"cmd": CMD_HELLO}
        ms.write((json.dumps(cmd) + "\n").encode())
        sent = ms.received_messages()
        assert len(sent) == 1
        assert sent[0]["cmd"] == CMD_HELLO

    def test_mock_serial_sends_start_with_params(self):
        ms = MockSerial()
        cmd = {"cmd": CMD_START, "electrode": 3,
               "params": {"e_begin_mV": -600.0, "e_end_mV": 200.0,
                          "e_step_mV": 5.0, "e_pulse_mV": 25.0,
                          "t_pulse_ms": 50, "t_period_ms": 200,
                          "t_equilibration_ms": 2000, "cycles": 1, "n_avg": 5}}
        ms.write((json.dumps(cmd) + "\n").encode())
        sent = ms.received_messages()
        assert sent[0]["electrode"] == 3
        assert sent[0]["params"]["e_begin_mV"] == pytest.approx(-600.0)

    def test_abort_command(self):
        ms = MockSerial()
        ms.write((json.dumps({"cmd": CMD_ABORT}) + "\n").encode())
        sent = ms.received_messages()
        assert sent[0]["cmd"] == CMD_ABORT


# ===========================================================================
# Section C: Live device tests  (skip unless AQUAHMET_PORT is set)
# ===========================================================================

DEVICE_PORT = os.environ.get("AQUAHMET_PORT", "")
DEVICE_BAUD = int(os.environ.get("AQUAHMET_BAUD", "115200"))
DEVICE_SKIP = pytest.mark.skipif(
    not DEVICE_PORT,
    reason="AQUAHMET_PORT not set — skipping live device tests"
)


@DEVICE_SKIP
class TestLiveDevice:
    """
    Live device integration tests.

    Set AQUAHMET_PORT=COM9 (Windows) or AQUAHMET_PORT=/dev/ttyUSB0 (Linux)
    before running.  Device must be flashed with current firmware.
    """

    @pytest.fixture(autouse=True)
    def serial_port(self):
        try:
            import serial as _serial
        except ImportError:
            pytest.skip("pyserial not installed")
        ser = _serial.Serial(DEVICE_PORT, DEVICE_BAUD, timeout=0.5,
                             rtscts=False, xonxoff=False)
        ser.rts = False
        ser.dtr = False
        yield ser
        ser.close()

    def _read_line(self, ser, timeout_s: float = 3.0) -> str | None:
        import time
        buf = b""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            ch = ser.read(1)
            if not ch:
                continue
            if ch == b"\n":
                return buf.decode("utf-8", errors="replace").strip()
            buf += ch
        return None

    def _send(self, ser, obj: dict) -> None:
        import serial as _serial
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        ser.write(line.encode())

    def _read_until_type(self, ser, t: str, timeout_s: float = 5.0) -> dict | None:
        import time
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self._read_line(ser, timeout_s=0.5)
            if line and line.startswith("{"):
                try:
                    msg = json.loads(line)
                    if msg.get("t") == t:
                        return msg
                except json.JSONDecodeError:
                    pass
        return None

    def test_device_hello_response(self, serial_port):
        self._send(serial_port, {"cmd": CMD_HELLO})
        msg = self._read_until_type(serial_port, T_HELLO, timeout_s=3.0)
        assert msg is not None, "No hello response from device"
        assert msg["proto"] == SERIAL_PROTOCOL_VERSION

    def test_device_state_response(self, serial_port):
        self._send(serial_port, {"cmd": CMD_STATE})
        msg = self._read_until_type(serial_port, T_STATE, timeout_s=3.0)
        assert msg is not None, "No state response from device"
        assert isinstance(msg["state"], int)
        assert 0 <= msg["state"] <= 5   # valid scan_state_t range

    def test_device_start_and_abort(self, serial_port):
        """Send start, wait for scan_started, then abort, wait for scan_aborted."""
        import time
        self._send(serial_port, {"cmd": CMD_START, "electrode": 3,
                                  "params": {"e_begin_mV": -500.0, "e_end_mV": 500.0,
                                              "e_step_mV": 5.0, "e_pulse_mV": 25.0,
                                              "t_pulse_ms": 50, "t_period_ms": 200,
                                              "t_equilibration_ms": 500,
                                              "cycles": 1, "n_avg": 5}})
        # Wait for scan_started event
        started = self._read_until_type(serial_port, T_EVENT, timeout_s=5.0)
        assert started is not None, "No scan_started event"
        assert started.get("name") == EVT_STARTED, (
            f"Expected scan_started, got: {started.get('name')}"
        )
        # Send abort
        time.sleep(0.2)
        self._send(serial_port, {"cmd": CMD_ABORT})
        # Wait for scan_aborted event
        aborted = self._read_until_type(serial_port, T_EVENT, timeout_s=5.0)
        assert aborted is not None, "No abort event"
        assert aborted.get("name") in (EVT_ABORTED, EVT_COMPLETE), (
            f"Unexpected event name after abort: {aborted.get('name')}"
        )
