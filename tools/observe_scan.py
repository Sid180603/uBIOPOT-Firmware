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
    ap.add_argument("--pulse", type=float, default=None,
                    help="DPV pulse amplitude in mV (default: firmware default 25)")
    ap.add_argument("--step", type=float, default=None,
                    help="DPV step size in mV (default: firmware default 5)")
    ap.add_argument("--period", type=int, default=None,
                    help="DPV period in ms (default: firmware default 200)")
    ap.add_argument("--e-begin", type=float, default=None, help="sweep start in mV")
    ap.add_argument("--e-end", type=float, default=None, help="sweep end in mV")
    ap.add_argument("--navg", type=int, default=None,
                    help="ADC averaging count (default: firmware default 5)")
    ap.add_argument("--t-pulse", type=int, default=None,
                    help="pulse width in ms (default: firmware default 50)")
    # --- CV (cyclic voltammetry) mode ---
    ap.add_argument("--cv", action="store_true",
                    help="run Cyclic Voltammetry instead of DPV")
    ap.add_argument("--cv-begin", type=float, default=None, help="CV start/return potential mV")
    ap.add_argument("--cv-vtx1", type=float, default=None, help="CV first vertex mV")
    ap.add_argument("--cv-vtx2", type=float, default=None, help="CV second vertex mV")
    ap.add_argument("--cv-step", type=float, default=None, help="CV step mV")
    ap.add_argument("--cv-rate", type=float, default=None, help="CV scan rate mV/s")
    ap.add_argument("--cv-cycles", type=int, default=None, help="CV number of cycles")
    ap.add_argument("--out", default=None, help="raw capture file (default: capture_<ts>.ndjson)")
    return ap.parse_args()


def main():
    a = parse_args()
    out_path = a.out or f"capture_{time.strftime('%Y%m%d_%H%M%S')}.ndjson"

    # Expected point count — differs for DPV (single sweep) vs CV (cycled loop).
    if a.cv:
        cb  = a.cv_begin  if a.cv_begin  is not None else -200.0
        v1  = a.cv_vtx1   if a.cv_vtx1   is not None else  600.0
        v2  = a.cv_vtx2   if a.cv_vtx2   is not None else -200.0
        cs  = a.cv_step   if a.cv_step   is not None else    5.0
        cyc = a.cv_cycles if a.cv_cycles is not None else    2
        seg = abs(v1 - cb) + abs(v2 - v1) + (abs(cb - v2) if v2 != cb else 0.0)
        expected_pts = int(round(seg / cs)) * cyc + 1
    else:
        E_BEGIN = a.e_begin if a.e_begin is not None else -900.0
        E_END   = a.e_end   if a.e_end   is not None else  500.0
        E_STEP  = a.step    if a.step    is not None else    5.0
        expected_pts = int(round(abs(E_END - E_BEGIN) / E_STEP)) + 1

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
        if a.cv:
            params = {}
            if a.cv_begin  is not None: params["e_begin_mV"]     = a.cv_begin
            if a.cv_vtx1   is not None: params["e_vertex1_mV"]   = a.cv_vtx1
            if a.cv_vtx2   is not None: params["e_vertex2_mV"]   = a.cv_vtx2
            if a.cv_step   is not None: params["e_step_mV"]      = a.cv_step
            if a.cv_rate   is not None: params["scan_rate_mV_s"] = a.cv_rate
            if a.cv_cycles is not None: params["cycles"]         = a.cv_cycles
            if a.navg      is not None: params["n_avg"]          = a.navg
            start_obj = {"cmd": "cv", "electrode": a.electrode}
            if params:
                start_obj["params"] = params
        else:
            start_obj = {"cmd": "start", "electrode": a.electrode}
            params = {}
            if a.pulse is not None:   params["e_pulse_mV"] = a.pulse
            if a.step is not None:    params["e_step_mV"] = a.step
            if a.period is not None:  params["t_period_ms"] = a.period
            if a.e_begin is not None: params["e_begin_mV"] = a.e_begin
            if a.e_end is not None:   params["e_end_mV"] = a.e_end
            if a.navg is not None:    params["n_avg"] = a.navg
            if a.t_pulse is not None: params["t_pulse_ms"] = a.t_pulse
            if params:
                start_obj["params"] = params
        cmd = json.dumps(start_obj)
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

    analyze(points, events, logs, hello, expected_pts, started, completed, stalled, a.electrode, out_path, a.cv)


def analyze(points, events, logs, hello, expected_pts, started, completed, stalled, electrode, out_path, cv=False):
    print("\n" + "=" * 60)
    print("ANALYSIS" + ("  (CV)" if cv else "  (DPV)"))
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

    # E monotonic (DPV only — a CV sweep legitimately reverses at each vertex)
    Es = [p.get("E") for p in points]
    dEs = [b - a for a, b in zip(Es, Es[1:])]
    non_mono = sum(1 for d in dEs if d <= 0)
    print(f"E range         : {min(Es):.1f} .. {max(Es):.1f} mV  (step ~{(dEs[0] if dEs else 0):.1f})")
    if non_mono and not cv:
        issues.append(f"E not strictly increasing: {non_mono} non-positive steps.")

    # I stats
    Is = [p.get("I") for p in points]
    imin, imax, imean = min(Is), max(Is), sum(Is) / len(Is)
    spread = imax - imin
    print(f"I (uA)          : min={imin:.4f} max={imax:.4f} mean={imean:.4f} spread={spread:.4f}")
    if spread < 1e-6:
        issues.append("Current is perfectly flat (0 spread) — ADC may not be sampling.")
    # dry cell: near-zero current is EXPECTED, so we do NOT flag small magnitudes.

    # --- Sign-alternating instability / oscillation detector ---
    # A real voltammogram is smooth: adjacent points differ little.  Analog
    # front-end (TIA) instability shows up as large, sign-flipping jumps between
    # adjacent points — often confined to the faradaic window — which is NOT a
    # peak.  Distinguish that from a genuine smooth peak using point-to-point
    # jitter and a count of "spike" points (opposite sign to both neighbours).
    dI = [b - a for a, b in zip(Is, Is[1:])]
    if len(dI) >= 4 and not cv:
        absdI = sorted(abs(d) for d in dI)
        med_jit = absdI[len(absdI) // 2]
        p95_jit = absdI[min(len(absdI) - 1, int(len(absdI) * 0.95))]
        thr = max(3.0 * med_jit, 5.0)   # "large" spike = 3x median jitter or 5 uA
        alt = 0
        for i in range(1, len(Is) - 1):
            if (Is[i] >  thr and Is[i - 1] < 0 and Is[i + 1] < 0) or \
               (Is[i] < -thr and Is[i - 1] > 0 and Is[i + 1] > 0):
                alt += 1
        print(f"pt-pt jitter    : median={med_jit:.2f}  p95={p95_jit:.2f} uA   sign-alt spikes={alt}")
        if alt >= 5 or p95_jit > 10.0:
            # Locate the window where instability concentrates (highest local jitter).
            win, worst, W = None, 0.0, min(8, len(dI))
            for i in range(len(dI) - W + 1):
                s = sum(abs(x) for x in dI[i:i + W]) / W
                if s > worst:
                    worst, win = s, (Es[i], Es[i + W - 1])
            loc = f" concentrated around E={win[0]:.0f}..{win[1]:.0f} mV" if win else ""
            issues.append(
                f"Sign-alternating instability ({alt} spikes, p95 pt-pt jump "
                f"{p95_jit:.1f} uA vs median {med_jit:.2f}){loc} — NOT a smooth DPV "
                f"curve; likely analog front-end (TIA) instability, not a peak.")

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
