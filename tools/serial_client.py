#!/usr/bin/env python3
"""
serial_client.py — Aqua-HMET P7 Reference Serial Client
=========================================================
Connects to the device over USB-serial (UART0 / COM port), speaks the P7
NDJSON protocol, live-prints data points, and saves a CSV on completion.

Usage
-----
    python tools/serial_client.py                    # auto-detect port, 115200 baud
    python tools/serial_client.py --port COM9        # Windows
    python tools/serial_client.py --port /dev/ttyUSB0  # Linux
    python tools/serial_client.py --port COM9 --baud 460800
    python tools/serial_client.py --port COM9 --start  # send CMD_START after hello
    python tools/serial_client.py --port COM9 --start --electrode 3
    python tools/serial_client.py --port COM9 --start \
        --e-begin -600 --e-end 200 --e-step 5 --e-pulse 25 \
        --t-pulse 50 --t-period 200 --t-equil 2000 --cycles 1

Protocol (P7 NDJSON)
--------------------
All messages are newline-delimited JSON (one object per line).
Lines not starting with '{' are silently ignored (may be IDF log output).

Device → host (output):
  {"t":"hello","fw":"1.0.0","proto":1,"device":"Aqua-HMET"}
  {"t":"point","e":1,"idx":0,"E":-500.0,"I":6.42,"RE":-495.0}
  {"t":"event","name":"scan_started","mode":"DPV","e":1}
  {"t":"event","name":"scan_complete"}
  {"t":"event","name":"scan_error","msg":"E_step must be > 0"}
  {"t":"state","state":0}
  {"t":"resync_complete","count":120,"state":0}

Host → device (commands):
  {"cmd":"hello"}
  {"cmd":"state"}
  {"cmd":"abort"}
  {"cmd":"start","electrode":1,"params":{...}}
  {"cmd":"zero"}

Field notes:
  "E"   : base potential in mV (e.g. -500.0 = -0.5 V)
  "I"   : differential current dI = I_pulse − I_base in µA
  "RE"  : measured RE voltage in mV
  "idx" : step index (0-based)
  "e"   : electrode number (1, 2, or 3)

Dependencies
------------
    pip install pyserial
"""

import argparse
import csv
import json
import signal
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed.  Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Protocol constants (mirror serial_comms_protocol.h)
# ---------------------------------------------------------------------------

PROTO_VERSION = 1

CMD_START  = "start"
CMD_ABORT  = "abort"
CMD_ZERO   = "zero"
CMD_STATE  = "state"
CMD_HELLO  = "hello"

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


# ---------------------------------------------------------------------------
# ANSI colour helpers (Windows cmd/PowerShell honour these in modern terminals)
# ---------------------------------------------------------------------------

def _c(code: str, text: str) -> str:
    """Wrap text in an ANSI escape sequence if stdout is a TTY."""
    if sys.stdout.isatty():
        return f"\033[{code}m{text}\033[0m"
    return text

GREEN  = lambda t: _c("32", t)
YELLOW = lambda t: _c("33", t)
CYAN   = lambda t: _c("36", t)
RED    = lambda t: _c("31", t)
DIM    = lambda t: _c("2",  t)
BOLD   = lambda t: _c("1",  t)


# ---------------------------------------------------------------------------
# Port auto-detection
# ---------------------------------------------------------------------------

CP21XX_VID = 0x10C4   # Silicon Labs CP210x (on most ESP32 DevKits)
CH34X_VID  = 0x1A86   # WCH CH340/CH341

def _auto_detect_port() -> str | None:
    """Return the first CP210x / CH340 serial port found, or None."""
    for info in serial.tools.list_ports.comports():
        if info.vid in (CP21XX_VID, CH34X_VID):
            return info.device
    return None


# ---------------------------------------------------------------------------
# CSV writer
# ---------------------------------------------------------------------------

def _make_csv_writer(path: Path):
    """Return (file_handle, csv.writer) with the standard Aqua-HMET CSV header."""
    fh = open(path, "w", newline="", encoding="utf-8")
    writer = csv.writer(fh)
    writer.writerow(["electrode", "idx", "E_mV", "I_uA", "RE_mV"])
    return fh, writer


# ---------------------------------------------------------------------------
# Session state
# ---------------------------------------------------------------------------

class ScanSession:
    """Accumulates data points for one scan."""

    def __init__(self):
        self.points: list[dict] = []
        self.started  = False
        self.complete = False
        self.aborted  = False
        self.error_msg: str | None = None

    def add_point(self, pt: dict) -> None:
        self.points.append(pt)

    def is_running(self) -> bool:
        return self.started and not (self.complete or self.aborted)


# ---------------------------------------------------------------------------
# Main client class
# ---------------------------------------------------------------------------

class AquaHMETClient:
    """Thin synchronous client for the Aqua-HMET P7 serial protocol."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 0.1):
        self._port_name = port
        self._baud      = baud
        self._timeout   = timeout
        self._ser: serial.Serial | None = None
        self._rx_buf    = b""

    def connect(self) -> None:
        """Open the serial port."""
        self._ser = serial.Serial(
            port     = self._port_name,
            baudrate = self._baud,
            bytesize = serial.EIGHTBITS,
            parity   = serial.PARITY_NONE,
            stopbits = serial.STOPBITS_ONE,
            timeout  = self._timeout,
            # RTS/DTR low to prevent accidental ESP32 reset
            rtscts   = False,
            xonxoff  = False,
        )
        self._ser.rts = False
        self._ser.dtr = False
        print(f"{GREEN('Connected')} to {BOLD(self._port_name)} at {self._baud} baud")

    def disconnect(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()
        print(DIM("Disconnected"))

    # ---- Low-level TX -------------------------------------------------------

    def send(self, obj: dict) -> None:
        """Serialise obj as JSON and send as one NDJSON line."""
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self._ser.write(line.encode("utf-8"))

    def send_hello(self) -> None:
        self.send({"cmd": CMD_HELLO})

    def send_start(self, electrode: int = 1, params: dict | None = None) -> None:
        msg: dict = {"cmd": CMD_START, "electrode": electrode}
        if params:
            msg["params"] = params
        self.send(msg)

    def send_abort(self) -> None:
        self.send({"cmd": CMD_ABORT})

    def send_state(self) -> None:
        self.send({"cmd": CMD_STATE})

    # ---- Low-level RX -------------------------------------------------------

    def _read_line(self) -> str | None:
        """Return the next complete NDJSON line, or None if nothing yet."""
        chunk = self._ser.read(256)
        if chunk:
            self._rx_buf += chunk
        nl = self._rx_buf.find(b"\n")
        if nl == -1:
            return None
        line = self._rx_buf[:nl].decode("utf-8", errors="replace").strip()
        self._rx_buf = self._rx_buf[nl + 1:]
        return line

    # ---- Message dispatch ---------------------------------------------------

    def _dispatch(self, line: str, session: ScanSession,
                  csv_writer=None, verbose: bool = False) -> None:
        """Parse one NDJSON line and update session state + print output."""
        if not line.startswith("{"):
            # Non-JSON: stray IDF log line — print dimmed and skip
            if verbose:
                print(DIM(f"  [log] {line}"))
            return

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print(YELLOW(f"  [bad JSON] {line}"))
            return

        t = msg.get("t", "")

        if t == T_HELLO:
            fw    = msg.get("fw", "?")
            proto = msg.get("proto", "?")
            dev   = msg.get("device", "Aqua-HMET")
            print(f"{BOLD('[hello]')} {dev}  fw={fw}  proto={proto}")

        elif t == T_POINT:
            elec = msg.get("e",   "?")
            idx  = msg.get("idx", "?")
            E_mV = msg.get("E",   float("nan"))
            I_uA = msg.get("I",   float("nan"))
            RE_mV= msg.get("RE",  float("nan"))
            # Print every 10th point to avoid terminal flood; always store all
            if isinstance(idx, int) and idx % 10 == 0:
                print(f"  {CYAN('pt')} e={elec} idx={idx:4d}  "
                      f"E={E_mV:8.2f} mV   I={I_uA:8.3f} µA   RE={RE_mV:8.2f} mV")
            session.add_point({"electrode": elec, "idx": idx,
                                "E_mV": E_mV, "I_uA": I_uA, "RE_mV": RE_mV})
            if csv_writer:
                csv_writer.writerow([elec, idx,
                                     f"{E_mV:.4f}", f"{I_uA:.4f}", f"{RE_mV:.4f}"])

        elif t == T_EVENT:
            name = msg.get("name", "")
            info = msg.get("msg",  "")
            if name == EVT_STARTED:
                session.started = True
                mode = msg.get("mode", "?")
                e    = msg.get("e",    "?")
                print(f"{GREEN('[scan_started]')} technique={mode}  electrode={e}")
            elif name == EVT_EQUILIB:
                print(f"  {DIM('[equilibrating]')} ...")
            elif name == EVT_COMPLETE:
                session.complete = True
                print(f"{GREEN('[scan_complete]')}  "
                      f"{len(session.points)} points collected")
            elif name == EVT_ABORTED:
                session.aborted = True
                print(f"{YELLOW('[scan_aborted]')}")
            elif name == EVT_ERROR:
                session.error_msg = info
                print(f"{RED('[scan_error]')} {info}")
            else:
                print(f"  [event] name={name}  {info or ''}")

        elif t == T_STATE:
            state_int = msg.get("state", -1)
            _STATE_NAMES = {0:"IDLE",1:"EQUILIBRATING",2:"RUNNING",
                            3:"COMPLETE",4:"ABORTING",5:"ERROR"}
            print(f"  [state] {_STATE_NAMES.get(state_int, str(state_int))}")

        elif t == T_PEAKS:
            peaks = msg.get("peaks", [])
            print(f"{BOLD('[peaks]')} {len(peaks)} peak(s) detected:")
            for pk in peaks:
                metal = pk.get("metal", "?")
                E_mV  = pk.get("E_mV",  float("nan"))
                I_uA  = pk.get("I_uA",  float("nan"))
                who   = pk.get("who_limit_ug_per_L", None)
                limit_str = f"  WHO ≤ {who} µg/L" if who else ""
                print(f"    {BOLD(metal):8s}  E={E_mV:.1f} mV   I={I_uA:.3f} µA{limit_str}")

        elif t == T_RESYNC:
            count = msg.get("count", "?")
            state = msg.get("state", "?")
            print(f"  [resync_complete] {count} points replayed  state={state}")

        elif t == T_ERROR:
            print(f"{RED('[error]')} {msg.get('msg', line)}")

        else:
            if verbose:
                print(f"  [?] {line}")

    # ---- High-level run loop ------------------------------------------------

    def run(self, *,
            auto_start: bool,
            electrode: int,
            params: dict | None,
            csv_out: Path | None,
            verbose: bool,
            idle_timeout_s: float = 60.0) -> ScanSession:
        """
        Main event loop.

        1. Send hello → wait for device hello response.
        2. Optionally send start command.
        3. Read + dispatch lines until scan finishes or idle_timeout_s elapses.
        4. Save CSV if requested.
        """
        session = ScanSession()
        csv_fh, csv_writer = (None, None)
        if csv_out:
            csv_fh, csv_writer = _make_csv_writer(csv_out)
            print(f"  CSV → {csv_out}")

        # Send hello; device will respond with its own hello + resync
        self.send_hello()
        print(DIM("  → {\"cmd\":\"hello\"} sent, waiting for device…"))

        if auto_start:
            # Small delay so the hello response comes first
            time.sleep(0.2)
            self.send_start(electrode=electrode, params=params)
            print(f"{DIM('  → start cmd sent for electrode')} {electrode}")

        last_data_time = time.monotonic()
        try:
            while True:
                line = self._read_line()
                if line:
                    last_data_time = time.monotonic()
                    self._dispatch(line, session,
                                   csv_writer=csv_writer, verbose=verbose)
                    if session.complete or session.aborted:
                        break
                else:
                    # Quiet period — check idle timeout
                    if time.monotonic() - last_data_time > idle_timeout_s:
                        print(YELLOW(f"\n[timeout] No data for {idle_timeout_s:.0f}s"))
                        break
        except KeyboardInterrupt:
            print(f"\n{YELLOW('[interrupted]')} Sending abort…")
            self.send_abort()
            # Drain for 0.5s to catch the scan_aborted event
            deadline = time.monotonic() + 0.5
            while time.monotonic() < deadline:
                line = self._read_line()
                if line:
                    self._dispatch(line, session, csv_writer=csv_writer, verbose=verbose)

        finally:
            if csv_fh:
                csv_fh.close()
                if session.points:
                    print(f"{GREEN('CSV saved:')} {csv_out}  ({len(session.points)} rows)")

        return session


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog        = "serial_client.py",
        description = "Aqua-HMET P7 reference serial client (NDJSON over UART0)",
        formatter_class = argparse.ArgumentDefaultsHelpFormatter,
    )
    # Connection
    p.add_argument("--port",  default=None,   help="Serial port (auto-detected if omitted)")
    p.add_argument("--baud",  type=int, default=115200, help="Baud rate")

    # Actions
    p.add_argument("--start",     action="store_true", help="Send CMD_START after hello")
    p.add_argument("--electrode", type=int, default=1,
                   choices=[0,1,2,3],
                   help="Electrode (0=all sequential, 1/2/3 single)")
    p.add_argument("--abort-only", action="store_true",
                   help="Send CMD_ABORT and exit (use to stop a running scan)")
    p.add_argument("--state-only", action="store_true",
                   help="Query engine state and exit")

    # DPV params (only used with --start)
    p.add_argument("--e-begin",  type=float, default=-500.0, metavar="mV")
    p.add_argument("--e-end",    type=float, default= 500.0, metavar="mV")
    p.add_argument("--e-step",   type=float, default=   5.0, metavar="mV")
    p.add_argument("--e-pulse",  type=float, default=  25.0, metavar="mV")
    p.add_argument("--t-pulse",  type=int,   default=    50, metavar="ms")
    p.add_argument("--t-period", type=int,   default=   200, metavar="ms")
    p.add_argument("--t-equil",  type=int,   default=  2000, metavar="ms")
    p.add_argument("--cycles",   type=int,   default=     1)
    p.add_argument("--n-avg",    type=int,   default=     5)

    # Output
    p.add_argument("--csv",     type=Path,    default=None,
                   help="Save scan data to CSV file (default: auto-timestamped)")
    p.add_argument("--no-csv",  action="store_true", help="Do not save a CSV")
    p.add_argument("--verbose", action="store_true",
                   help="Print non-JSON log lines and unrecognised messages")
    p.add_argument("--timeout", type=float,   default=300.0,
                   metavar="s", help="Idle timeout in seconds")
    return p


def main() -> int:
    args = _build_parser().parse_args()

    # ---- Port resolution ----
    port = args.port
    if port is None:
        port = _auto_detect_port()
        if port:
            print(f"{DIM('Auto-detected:')} {port}")
        else:
            print(RED("ERROR: No ESP32 serial port found.  "
                      "Use --port to specify one."), file=sys.stderr)
            return 1

    client = AquaHMETClient(port=port, baud=args.baud)
    try:
        client.connect()
    except serial.SerialException as exc:
        print(RED(f"ERROR: Cannot open {port}: {exc}"), file=sys.stderr)
        return 1

    # ---- Quick commands ----
    if args.abort_only:
        client.send_abort()
        print(DIM("Abort sent."))
        client.disconnect()
        return 0

    if args.state_only:
        client.send_state()
        # Read a few lines to print the state reply
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            line = client._read_line()
            if line and line.startswith("{"):
                client._dispatch(line, ScanSession(), verbose=args.verbose)
                if '"state"' in line:
                    break
        client.disconnect()
        return 0

    # ---- Full scan run ----
    params = {
        "e_begin_mV":         args.e_begin,
        "e_end_mV":           args.e_end,
        "e_step_mV":          args.e_step,
        "e_pulse_mV":         args.e_pulse,
        "t_pulse_ms":         args.t_pulse,
        "t_period_ms":        args.t_period,
        "t_equilibration_ms": args.t_equil,
        "cycles":             args.cycles,
        "n_avg":              args.n_avg,
    }

    csv_path: Path | None = None
    if not args.no_csv:
        csv_path = args.csv or Path(
            f"aquahmet_scan_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        )

    session = client.run(
        auto_start    = args.start,
        electrode     = args.electrode,
        params        = params if args.start else None,
        csv_out       = csv_path,
        verbose       = args.verbose,
        idle_timeout_s= args.timeout,
    )
    client.disconnect()

    # ---- Summary ----
    print()
    print(BOLD("=== Session summary ==="))
    print(f"  Points collected : {len(session.points)}")
    if session.complete:
        print(f"  Status           : {GREEN('COMPLETE')}")
    elif session.aborted:
        print(f"  Status           : {YELLOW('ABORTED')}")
    elif session.error_msg:
        print(f"  Status           : {RED('ERROR')} — {session.error_msg}")
    else:
        print(f"  Status           : {YELLOW('TIMEOUT / IDLE')}")

    return 0 if (session.complete or not args.start) else 1


if __name__ == "__main__":
    sys.exit(main())
