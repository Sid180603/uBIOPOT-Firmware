# Aqua-HMET Firmware

ESP-IDF firmware for the Aqua-HMET DPV heavy metal detector (BITS Pilani, 2025–2026).

**Hardware:** ESP32-WROOM-32 + MCP4921 12-bit SPI DAC + ADS1115 16-bit I²C ADC + 3× CD4066 electrode mux + ILI9341 2.4" TFT.

**Default technique:** DPV (Differential Pulse Voltammetry). Architecture supports CV/LSV/SWV/NPV via the technique registry (to be added post-publish).

**Three synchronized UIs:** animated on-device TFT (LVGL) + WiFi web app (WebSocket SPA) + USB serial (NDJSON).

---

## Architecture

```
components/
  echem_core/    — Pure C, no IDF/FreeRTOS. DPV algo, calibration, peak finder, metal ID,
                   WHO limits, protocol types. Compiles on HOST (PC) for unit testing.
  pstat_hal/     — HAL: MCP4921 DAC, ADS1115 ADC (continuous GAIN_ONE), CD4066 mux, buttons, LEDs.
  acq_engine/    — FreeRTOS: Core-1 AcquisitionTask, data queue, engine API, server-auth buffer.
  ui_tft/        — LVGL on-device UI: ILI9341 display, 2-button encoder navigation, live line chart.
  net_comms/     — WiFi (SoftAP + STA), captive portal, mDNS, HTTP server, WebSocket binary protocol.
  serial_comms/  — USB serial NDJSON protocol (UART0), engine sink, command parser, parity with web.
host_test/       — Standalone CMake + Unity. Tests echem_core on host GCC (no hardware needed).
web/             — SPA source (Vite + uPlot). Built output gzip-compressed → LittleFS image.
                   Includes mock_backend.js (Node WS mock), Playwright E2E tests, potentiostat-core.js.
tools/           — serial_client.py (reference Python CLI) + observe_scan.py (DPV scan capture + anomaly analysis).
chips/           — Custom Wokwi chips: MCP4921 (SPI DAC) + ADS1115 (I²C ADC with Gaussian cell model).
```

---

## Hardware Pin Map

| Signal | GPIO | Notes |
|--------|------|-------|
| TFT SCLK | 15 | HSPI CLK · strapping pin (HIGH at boot — internal pull-up) |
| TFT MOSI | 2 | HSPI MOSI · strapping pin (must be LOW/float at boot) · also onboard LED |
| TFT CS | 5 | Must be HIGH at boot (strapping) |
| TFT DC | 4 | Data/Command |
| TFT RST | — | Tied to ESP32 EN — software reset via SPI cmd 0x01 |
| TFT MISO | — | Not connected (write-only panel) |
| TFT Backlight | — | Hardwired to 3V3 (always on, no PWM) |
| DAC SCK | 18 | MCP4921, SPI3_HOST (VSPI) |
| DAC MOSI | 23 | MCP4921 |
| DAC CS | 26 | MCP4921 |
| ADC SDA | 21 | ADS1115, I²C 400 kHz |
| ADC SCL | 22 | ADS1115 |
| Mux T1 | 32 | Electrode 1 (CD4066) |
| Mux T2 | 25 | Electrode 2 (CD4066) |
| Mux T3 | 33 | Electrode 3 (CD4066) |
| Start button | 14 | Active-low, INPUT_PULLUP |
| Nav/Reset btn | 0 | Active-low · short = navigate/back · long 10 s = factory-reset WiFi creds · STRAPPING PIN |
| Ready LED | 12 | Strapping pin (VDD_SDIO) — must NOT be HIGH at reset |
| Processing LED | 13 | |

---

## Prerequisites

- [ESP-IDF v5.4.4](https://github.com/espressif/esp-idf/releases/tag/v5.4.4) (exact version — pinned for reproducibility)
- CMake ≥ 3.16, Ninja or GNU Make
- For host tests: GCC/Clang on host (Linux, macOS, or WSL/MinGW on Windows)

---

## Build (firmware)

    . $IDF_PATH/export.sh          # activate IDF environment
    idf.py set-target esp32
    idf.py build

After the first build, commit the generated `dependencies.lock`:

    git add dependencies.lock
    git commit -m "chore: lock component manager dependencies"

---

## Unit Tests (host — no hardware required)

    cmake -B build_host host_test
    cmake --build build_host
    ctest --test-dir build_host --output-on-failure

> The first run fetches Unity from GitHub via CMake FetchContent. Internet access is required.
> Behind a proxy: set `GIT_PROXY` in your environment, or clone Unity manually into `host_test/vendor/Unity`
> and update `host_test/CMakeLists.txt` to use `add_subdirectory` instead of `FetchContent`.

---

## Flash

    idf.py -p <PORT> flash monitor

Default electrode is **Electrode 3** (configurable from the TFT home screen or web UI).

---

## Phase Progress

| Phase | Status | Description |
|-------|--------|-------------|
| P0 | ✅ Done | Scaffolding — repo, build system, component skeletons, host tests, CI |
| P1 | ✅ Done | HAL + drivers (MCP4921 SPI DAC, ADS1115 I²C ADC, CD4066 mux, iot_button, LEDs, selftest) — **selftest PASS on real ESP32-D0WD-V3** |
| P2 | ✅ Done | Electrochemistry core + full DPV algorithm + Unity host tests (all 4 suites green) |
| P3 | ✅ Done | Acquisition engine (FreeRTOS Core-1 AcqTask, Core-0 Dispatcher, server-auth buffer, sink API) |
| P4 | ✅ Done | On-device LVGL UI (ILI9341, 2-button encoder nav, 6 screens, live voltammogram, engine sink) — **display validated on real hardware** |
| P5 | ✅ Done | Connectivity (WiFi SoftAP+STA, captive portal, mDNS, WebSocket, HTTP API, LittleFS) |
| P6 | ✅ Done | Web SPA (uPlot, aqua-metal light theme, live chart, CSV export/import overlay, 28 Playwright E2E tests) |
| P7 | ✅ Done | USB serial protocol (NDJSON on UART0, parity with web, serial_client.py reference CLI, Wokwi custom chips) — **publishable milestone** |
| P8 | ⏳ Next | Persistence + calibration (NVS, auto-zero, bench calibration, concentration slopes, WHO threshold display) |
| P9 | ⏳ | Integration + hardware validation vs commercial instrument + Web Serial standalone + perf profiling + thesis figures |

**Publishable milestone (reached):** end of P7 — DPV working across TFT + WiFi web + USB serial simultaneously.

**First DPV scan on real hardware (2026-07-12):** 281/281 points, scan_complete delivered, results screen loaded. Dry cell (open circuit) — flat ~0 µA as expected. Validated the full data pipeline from DAC/ADC through all three UIs.

**Build size (P7):** `aquahmet.bin` 1292 KB (firmware) + `littlefs.bin` 1000 KB (web SPA) + bootloader 25 KB + partition table 3 KB. Total flash usage: ~2.3 MB of 4 MB.

---

## CI (7 jobs — all green)

| # | Job | What it verifies |
|---|-----|-----------------|
| 1 | Firmware Build | ESP-IDF v5.4.4 compile (esp32 target) |
| 2 | Host Unit Tests | echem_core on Ubuntu GCC + purity check |
| 3 | Protocol Conformance | P5 WebSocket pytest + net_comms_protocol.h purity |
| 4 | Playwright E2E | P6 SPA headless Chromium (28 tests) |
| 5 | Serial Protocol | P7 NDJSON pytest + serial_comms_protocol.h parity |
| 6 | Serial PTY E2E | P7 pipe-based MockDevice E2E pytest |
| 7 | Wokwi Simulation | L7 full-device sim with custom WASM chips (MCP4921 + ADS1115) |

---

## Dev Loop (no hardware required)

| Level | Command | What it proves |
|-------|---------|----------------|
| **1 — Node mock** | `cd web && node mock_backend.js --port 3000` | Full SPA live in browser; Cd/Pb/Cu Gaussian peaks stream over real WS binary protocol |
| **2 — Playwright** | `cd web && npm install && npm test` | 28 automated E2E tests — WS connect, form validation, binary frames, scan complete, CSV export, abort, resync, reference import, scroll/nav API |
| **3 — pytest (web)** | `pytest test/test_p5_protocol.py -v -k "not device"` | Wire-protocol unit tests (frame encoding, JSON schema, CSV format) — no browser |
| **4 — pytest (serial)** | `pytest test/test_p7_serial.py test/test_p7_pty.py -v` | NDJSON serial protocol conformance + pipe-based MockDevice E2E |
| **5 — Wokwi** | CI or `wokwi-cli` with `WOKWI_CLI_TOKEN` | Real compiled firmware in simulator with custom DAC + ADC chips |

Build the SPA (`npm run build`) before running the mock server or Playwright tests — the mock serves from `web/dist/`.

See **[`Dev-Env.md`](Dev-Env.md)** for the complete walkthrough.

---

## Observe a Real DPV Scan

`tools/observe_scan.py` connects to the device over USB serial, optionally triggers a scan, records the full NDJSON stream, and prints an automated anomaly report.

    py -3.12 tools/observe_scan.py --port COM9 --electrode 3          # trigger + capture
    py -3.12 tools/observe_scan.py --port COM9 --observe-only         # listen only (trigger by button)

Checks: point count, idx contiguity, E monotonicity, I/RE ranges, scan_complete delivery, WDT/crash logs. Stall detection auto-fires if no new point arrives within `--stall` seconds.

---

## WiFi Connection (Phone or Computer)

The ESP32 runs a WiFi hotspot (`Aqua-HMET-<last 4 MAC digits>`, WPA2). Connect your phone or laptop to it.

A captive portal intercepts DNS and pops the SPA in your browser automatically on iOS, Android, and Windows — no URL to type.

Manual access: `http://192.168.4.1`

The WebSocket streams live DPV data at `ws://192.168.4.1/ws` (binary DataPoint frames, 16 bytes LE).

---

## On-device DPV Scan Behavior

The TFT scan screen shows a live LVGL line chart (I vs E) that updates at ~2 fps during the scan. A progress counter (`42/281`) and elapsed timer tick in real time. The scan sweeps from E_begin to E_end in 5 mV steps (~60 s for a full -900 to +500 mV sweep).

**Memory-constrained rendering:** The ESP32-WROOM-32 has no PSRAM. WiFi + LVGL + SPI display rendering compete for ~233 KB of internal RAM. Key optimizations:
- Line chart (not scatter) — ~8x faster rendering, prevents LVGL task from saturating Core 0
- Flush timer throttle — chart data inserted every 500 ms (~2 fps), not per-point
- Display refresh throttle — LVGL refresh period set to 500 ms during scans
- WiFi buffer reduction + IPv6 disabled — reclaims ~20 KB heap
- Single-buffered 20-line SPI draw buffer — halves DMA allocation vs double-buffered
- Lazy screen creation — only splash + home at boot; scan/results/settings created on navigate

**Abort:** Long-press the START button during a scan. The scan aborts, the TFT returns to the home screen, and the button release is suppressed (no accidental restart).

---

## P4 On-device UI — Dev Loop (LVGL PC Simulator)

All screen layout and animation work happens on the PC **before** flashing — rebuild takes < 2 s vs a 30–60 s flash cycle.

```
sim/
  lv_conf.h      — LVGL9 config for PC (LV_STDLIB_CLIB, depth-16, Montserrat 14/20/28)
  main_sim.c     — Complete simulator entry point; mirrors original lv_port_pc_vscode/src/main.c
  stubs/         — Lightweight IDF header replacements (esp_log, esp_err, acq_engine, freertos)
  WSL_SETUP.md   — Full step-by-step build guide (verified on WSL2 Debian + WSLg)
  README.md      — Overview
```

See **[`sim/WSL_SETUP.md`](sim/WSL_SETUP.md)** for the complete, verified setup procedure.

**Verified working (2026-07-03):** splash → home → scan-live → results flow, Tab/Enter/two-finger-scroll
navigation, synthetic DPV Gaussian voltammogram (Pb²⁺ peak at −400 mV), toast on About.

**What the sim proves:** screen layout, navigation flow, chart animation, transitions, fonts, colours.
**What only the real board proves:** RGB565 byte-swap colour accuracy, 40 MHz SPI refresh smoothness,
RAM budget under WiFi + LVGL simultaneously, physical button ergonomics.

> **Validated on hardware (2026-07-05):** LVGL renders correctly on the ILI9341 at 40 MHz once the allocator
> (CLIB) and orientation (`lvgl_port` rotation) were fixed — see **Hardware bring-up** below.

---

## Hardware Bring-up (2026-07-05 · ESP32-D0WD-V3)

First on-silicon validation of P1–P4:

- **HAL selftest PASS** — MCP4921 DAC ramp, ADS1115 (`0x48`) reads, CD4066 mux cycle (T1→T2→T3), LED drive.
- **TFT UI renders** — splash → home at 40 MHz SPI, correct orientation.

Two firmware fixes were required for on-device operation (neither reproduces in the PC sim, which uses CLIB
malloc + a huge host stack):

1. **LVGL allocator** — the builtin 64 KB TLSF pool OOM'd while building 5 screens + Montserrat 14/20/28
   (`lv_realloc` → NULL → assert spin → task-WDT reboot loop). Fixed with `CONFIG_LV_USE_CLIB_MALLOC=y` so
   LVGL uses the full ESP-IDF heap (~180 KB). Main task stack also raised to 16384.
2. **TFT orientation** — `esp_lvgl_port` re-applies `disp_cfg.rotation` to the panel via `esp_lcd`, overriding
   any manual `esp_lcd_panel_swap_xy/mirror`. Set orientation **only** in `lvgl_port_display_cfg_t.rotation`
   (`swap_xy/mirror_x/mirror_y = true` for this panel/mount) — manual MADCTL calls cause a stride-shear image.

---

## On-device Bug Fixes (2026-07-12 · first DPV scan debugging)

Bugs found and fixed during the first real DPV scan on hardware:

| Bug | Root cause | Fix |
|-----|-----------|-----|
| Button release after abort restarts scan | LVGL `CLICKED` event leaks across screen transition | `lv_indev_wait_release(s_indev)` after abort/error navigation |
| Progress shows `0/---` | `scr_scan_set_progress()` had zero callers | Wired flush timer counter + `scr_scan_set_total_steps()` at scan start |
| Task WDT fires during scan (IDLE0 starved) | Scatter chart + infinite animations saturate Core 0 | Line chart, animations removed, `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` |
| `scan_complete` never delivered | Dispatcher blocked on LVGL lock (priority inversion) | Per-event lock strategy: 1s timeout for terminal events, 100ms for non-terminal |
| TFT stuck on scan screen | Lock timeout → `if(locked)` skipped screen transition | Terminal events use 1s timeout (sufficient for line chart frame gap) |

---

## P1 Bench Bringup Checklist

Flash with `CONFIG_UBIOPOT_SELFTEST_MODE=y` (menuconfig → Aqua-HMET → Dev/Debug):

    idf.py menuconfig          # enable UBIOPOT_SELFTEST_MODE
    idf.py -p <PORT> flash monitor

Selftest sequence (automated, ~40 s):
1. **LED blink** — READY + PROCESSING alternate × 3 (visual)
2. **DAC ramp** — steps 0→4095 in 512 increments, prints `Vout_expected` — verify with DMM at MCP4921 Vout pin
3. **ADC reads** — 10 samples from AIN1 (current) + AIN0 (voltage), logs µA and V
4. **Mux cycle** — T1→T2→T3→off, 100 ms each — verify with oscilloscope (only one HIGH at a time)
5. **Button wait** — 10 s window; press START (GPIO14) or NAV (GPIO0) to log debounce events

**P1 DoD** (hardware bench) — selftest **PASS** on ESP32-D0WD-V3 (2026-07-05):
- [x] `ADS1115 ADC init OK` (`0x48` ACK) in serial log
- [x] DAC ramp 0→4095 executed (selftest log) — DMM linearity check pending on final wired unit
- [x] ADS1115 reads current (AIN1) + voltage (AIN0) channels
- [x] Mux cycles T1→T2→T3 — scope isolation check pending on final unit
- [ ] START + NAV buttons — **unwired on the demo unit** (hardware, not firmware); verify on final build
- [x] READY LED = GPIO12 boots clean (no strapping fault); LED visibility pending on final wired unit

---

## What "superior" means (concrete fixes vs original firmware)

| Original bug / limitation | Fix in v2 |
|---|---|
| Internal 8-bit DAC (256 steps, ~13 mV/step) | External MCP4921 12-bit DAC (~1 mV/step) |
| Internal 12-bit ADC + 4th-order poly correction | External ADS1115 16-bit, linear, GAIN\_ONE (125 nA/bit with 1 kΩ TIA) |
| ADS1115 gain never set → 0.1875 mV/bit (paper claims 62.5 µV/bit) | `ads_set_gain(GAIN_ONE)` → 0.125 mV/bit, best single-ended fit |
| Same voltage polynomial applied to current channel (copy-paste bug) | Separate calibrated `calib_vout_to_current_uA()` |
| `cycles` member never resets → 2nd DPV run does nothing | Stateless re-entrant algorithm |
| `delay(0.01)` truncates to `delay(0)` → averaging meaningless | Real timed averaging with timing compensation |
| Voltage reconstructed from DAC count | Measured at ADS1115 AIN0 (RE readback) |
| Fully blocking architecture, no abort | FreeRTOS Core-1 AcquisitionTask, instant abort flag |
| No streaming protocol | Server-authoritative buffer + fan-out to 3 sinks (TFT / WS / serial) |
| Single LSV technique hardcoded | Technique registry (DPV default; CV/LSV/SWV/NPV slots post-publish) |

---

## License

MIT — see [LICENSE](LICENSE).
