#!/usr/bin/env python3
"""
observe_scan.py — drive a DPV scan over USB serial and auto-analyze it for bugs.

Connects to the Aqua-HMET device over its NDJSON serial protocol (UART0 @ 115200),
optionally triggers a scan, records every line to a capture file, and prints an
anomaly report (point count, E monotonicity, idx contiguity, I/RE ranges, events,
and any firmware log warnings/resets).

Usage (PowerShell):
    py -3.12 tools/observe_scan.py --port COM9 --electrode 3
    py -3.12 tools/observe_scan.py --port COM9 --observe-only   # just listen, no start
"""
import argparse
import json
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed. Run:  py -3.12 -m pip install pyserial")


def parse_args():
    ap = argparse.ArgumentParser(description="Observe + analyze a DPV scan over serial.")
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--electrode", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="max seconds to wait for scan_complete")
    ap.add_argument("--stall", type=float, default=10.0,
                    help="seconds with no new point before declaring a stall")
    ap.add_argument("--observe-only", action="store_true",
                    help="do not send start; just listen (e.g., trigger scan by button)")
    ap.add_argument("--out", default=None, help="raw capture file (default: capture_<ts>.ndjson)")
    return ap.parse_args()


def main():
    a = parse_args()
    out_path = a.out or f"capture_{time.strftime('%Y%m%d_%H%M%S')}.ndjson"

    # Expected sweep for default params (e_begin -900, e_end 500, step 5)
    E_BEGIN, E_END, E_STEP = -900.0, 500.0, 5.0
    expected_pts = int(round(abs(E_END - E_BEGIN) / E_STEP)) + 1  # 281

    print(f"[open] {a.port} @ {a.baud}")
    try:
        ser = serial.Serial()
        ser.port = a.port
        ser.baudrate = a.baud
        ser.timeout = 0.2
        # Do NOT drive DTR/RTS -> avoids yanking EN/IO0 and resetting the ESP32 on open.
        ser.dtr = False
        ser.rts = False
        ser.open()
    except serial.SerialException as e:
        sys.exit(f"[fail] cannot open {a.port}: {e}")

    points = []          # list of dicts: {e, idx, E, I, RE}
    events = []          # list of (name, msg)
    logs = []            # non-JSON lines (firmware logs)
    hello = None
    raw = open(out_path, "w", encoding="utf-8")

    def drain(seconds):
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += ser.read(256)
        return buf

    # Settle + clear any boot noise.
    time.sleep(0.4)
    ser.reset_input_buffer()

    # Handshake
    ser.write(b'{"cmd":"hello"}\n')

    if not a.observe_only:
        # small gap so hello reply isn't interleaved with the start burst
        time.sleep(0.3)
        # Clear any previously stuck/running scan so engine_start isn't rejected.
        ser.write(b'{"cmd":"abort"}\n')
        time.sleep(0.6)
        cmd = json.dumps({"cmd": "start", "electrode": a.electrode})
        ser.write((cmd + "\n").encode())
        print(f"[send] {cmd}")

    print(f"[capture] -> {out_path}   (Ctrl+C to stop early)\n")

    started = False
    completed = False
    stalled = False
    deadline = time.time() + a.timeout
    line_buf = b""
    last_progress = 0
    last_point_time = time.time()

    def handle_line(text):
        nonlocal hello, started, completed, last_progress, last_point_time
        raw.write(text + "\n"); raw.flush()
        if not text.startswith("{"):
            logs.append(text)
            low = text.lower()
            if any(k in low for k in ("error", "warn", "guru", "rst:", "brownout",
                                      "assert", "abort() was called", "watchdog", "wdt")):
                print(f"[LOG!] {text}")
            return
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            logs.append(text)
            return
        t = msg.get("t")
        if t == "point":
            points.append(msg)
            last_point_time = time.time()
            if len(points) - last_progress >= 20:
                last_progress = len(points)
                p = points[-1]
                pct = 100.0 * len(points) / max(expected_pts, 1)
                print(f"  ..{len(points):4d} pts ({pct:5.1f}%)  E={p.get('E'):.1f}mV "
                      f"I={p.get('I'):.3f}uA RE={p.get('RE'):.1f}mV")
        elif t == "event":
            name = msg.get("name", "?")
            events.append((name, msg.get("msg", "")))
            print(f"[event] {name}{('  ' + msg['msg']) if msg.get('msg') else ''}")
            if name == "scan_started":
                started = True
            if name in ("scan_complete", "scan_aborted", "scan_error"):
                completed = True
        elif t == "hello":
            hello = msg
            print(f"[hello] fw={msg.get('fw')} proto={msg.get('proto')} dev={msg.get('device')}")
        elif t == "state":
            print(f"[state] engine_state={msg.get('state')}")
        elif t == "error":
            events.append(("error", msg.get("msg", "")))
            print(f"[ERROR] {msg.get('msg')}")

    def drain_lines():
        nonlocal line_buf
        while b"\n" in line_buf:
            one, line_buf = line_buf.split(b"\n", 1)
            text = one.decode("utf-8", errors="replace").strip("\r\n ")
            if text:
                handle_line(text)

    try:
        while time.time() < deadline:
            chunk = ser.read(512)
            if chunk:
                line_buf += chunk
                drain_lines()
            if completed:
                break
            # Stall detection: scan started, no new point for --stall seconds.
            if started and (time.time() - last_point_time) > a.stall:
                stalled = True
                p = points[-1] if points else None
                pct = 100.0 * len(points) / max(expected_pts, 1)
                print(f"\n[STALL] no new point for {a.stall:.0f}s at {len(points)} pts ({pct:.1f}%)"
                      + (f", last E={p.get('E'):.1f}mV idx={p.get('idx')}" if p else ""))
                # Ask the engine what state it thinks it's in + grab any trailing logs.
                ser.write(b'{"cmd":"state"}\n')
                tail = time.time() + 2.0
                while time.time() < tail:
                    c = ser.read(256)
                    if c:
                        line_buf += c
                drain_lines()
                break
    except KeyboardInterrupt:
        print("\n[stop] interrupted by user")
    finally:
        raw.close()
        ser.close()

    analyze(points, events, logs, hello, expected_pts, started, completed, stalled, a.electrode, out_path)


def analyze(points, events, logs, hello, expected_pts, started, completed, stalled, electrode, out_path):
    print("\n" + "=" * 60)
    print("ANALYSIS")
    print("=" * 60)
    issues = []

    if hello is None:
        issues.append("No hello reply — device may not be responding on this port/baud.")

    if not started:
        issues.append("No 'scan_started' event seen.")
    if stalled:
        n = len(points)
        pct = 100.0 * n / max(expected_pts, 1)
        loc = ""
        if points:
            p = points[-1]
            loc = f" at idx={p.get('idx')} E={p.get('E'):.1f}mV I={p.get('I'):.4f}uA"
        issues.append(f"SCAN STALLED at {n} pts ({pct:.1f}%){loc} — hang reproduced.")
    elif not completed:
        issues.append("Scan did not finish (no scan_complete/aborted/error before timeout).")

    n = len(points)
    print(f"points captured : {n}  (expected ~{expected_pts} for default sweep)")
    if n == 0:
        issues.append("Zero data points captured.")
        _report(issues, events, logs)
        return

    if abs(n - expected_pts) > 2:
        issues.append(f"Point count {n} deviates from expected {expected_pts}.")

    # electrode field
    elecs = {p.get("e") for p in points}
    print(f"electrode field : {sorted(elecs)}")
    if elecs != {electrode}:
        issues.append(f"Electrode field mismatch: saw {sorted(elecs)}, expected {{{electrode}}}.")

    # idx contiguity
    idxs = [p.get("idx") for p in points]
    exp = list(range(len(idxs)))
    if idxs != exp:
        # find first divergence
        bad = next((i for i, (a, b) in enumerate(zip(idxs, exp)) if a != b), None)
        issues.append(f"idx not contiguous 0..N-1 (first divergence at position {bad}: got {idxs[bad] if bad is not None else '?'}).")

    # E monotonic
    Es = [p.get("E") for p in points]
    dEs = [b - a for a, b in zip(Es, Es[1:])]
    non_mono = sum(1 for d in dEs if d <= 0)
    print(f"E range         : {min(Es):.1f} .. {max(Es):.1f} mV  (step ~{(dEs[0] if dEs else 0):.1f})")
    if non_mono:
        issues.append(f"E not strictly increasing: {non_mono} non-positive steps.")

    # I stats
    Is = [p.get("I") for p in points]
    imin, imax, imean = min(Is), max(Is), sum(Is) / len(Is)
    spread = imax - imin
    print(f"I (uA)          : min={imin:.4f} max={imax:.4f} mean={imean:.4f} spread={spread:.4f}")
    if spread < 1e-6:
        issues.append("Current is perfectly flat (0 spread) — ADC may not be sampling.")
    # dry cell: near-zero current is EXPECTED, so we do NOT flag small magnitudes.

    # RE vs E deviation
    REs = [p.get("RE") for p in points]
    devs = [abs(re - e) for re, e in zip(REs, Es)]
    print(f"|RE-E| dev (mV) : min={min(devs):.1f} max={max(devs):.1f} mean={sum(devs)/len(devs):.1f}")

    # log anomalies
    flagged = [l for l in logs if any(k in l.lower() for k in
               ("error", "warn", "guru", "rst:", "brownout", "assert", "watchdog", "wdt"))]
    if flagged:
        issues.append(f"{len(flagged)} suspicious firmware log line(s) — see below.")

    # event sequence
    names = [e[0] for e in events]
    print(f"events          : {names}")
    if "scan_error" in names:
        issues.append("scan_error event present.")

    _report(issues, events, logs, flagged)
    print(f"\nraw capture saved: {out_path}")


def _report(issues, events, logs, flagged=None):
    print("-" * 60)
    if flagged:
        print("Suspicious log lines:")
        for l in flagged:
            print("   " + l)
    if issues:
        print(f"\n[RESULT] {len(issues)} potential issue(s):")
        for i, s in enumerate(issues, 1):
            print(f"  {i}. {s}")
    else:
        print("\n[RESULT] No anomalies detected — scan looks healthy.")


if __name__ == "__main__":
    main()
