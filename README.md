# uBIOPOT Firmware v2

ESP-IDF rewrite of the uBIOPOT multiplexed potentiostat firmware (BITS Pilani, *Lab on a Chip* 2025).

**Hardware:** ESP32-WROOM-32 + MCP4921 12-bit SPI DAC + ADS1115 16-bit I²C ADC + 3× CD4066 electrode mux + ILI9341 2.4" TFT.

**Default technique:** DPV (Differential Pulse Voltammetry). Architecture supports CV/LSV/SWV/NPV via the technique registry (to be added post-publish).

**Three synchronized UIs:** animated on-device TFT (LVGL) + WiFi web app (WebSocket SPA) + USB serial (NDJSON).

---

## Architecture

```
components/
  echem_core/   — Pure C, no IDF/FreeRTOS. DPV algo, calibration, peak finder, protocol types.
                  Compiles on HOST (PC) for unit testing. The testability keystone.
  pstat_hal/    — HAL: MCP4921 DAC, ADS1115 ADC (continuous GAIN_ONE), CD4066 mux, buttons, LEDs.
  acq_engine/   — FreeRTOS: Core-1 AcquisitionTask, data queue, engine API, server-auth buffer.
  ui_tft/       — LVGL on-device UI: ILI9341 display, 2-button encoder navigation, live chart.
  net_comms/    — WiFi (SoftAP + STA), captive portal, mDNS, HTTP server, WebSocket (P5).
host_test/      — Standalone CMake + Unity. Tests echem_core on host GCC (no hardware needed).
web/            — SPA source (Vite → minify → gzip → LittleFS image, built in P6).
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

---

## Phase Progress

| Phase | Status | Description |
|-------|--------|-------------|
| P0 | ✅ Done | Scaffolding — repo, build system, component skeletons, host tests, CI |
| P1 | ✅ Done | HAL + drivers (MCP4921 SPI DAC, ADS1115 I²C ADC, CD4066 mux, iot_button, LEDs, selftest) |
| P2 | ✅ Done | Electrochemistry core + full DPV algorithm + Unity host tests (all 4 suites green) |
| P3 | ✅ Done | Acquisition engine (FreeRTOS Core-1 AcqTask, Core-0 Dispatcher, server-auth buffer, sink API) |
| P4 | ✅ Done | On-device LVGL UI (ILI9341, 2-button encoder nav, 6 screens, live voltammogram, engine sink) |
| P5 | ⏳ | Connectivity (WiFi SoftAP+STA, captive portal, mDNS, WebSocket, HTTP API, LittleFS) |
| P6 | ⏳ | Web SPA (uPlot, dark theme, live chart, CSV export/import overlay, Playwright tests) |
| P7 | ⏳ | USB serial protocol (NDJSON, parity with web) — **publishable milestone** |
| P8 | ⏳ | Persistence + calibration (NVS, auto-zero, bench calibration, concentration slopes) |
| P9 | ⏳ | Integration + hardware validation vs commercial instrument + perf profiling + thesis figures |

**Publishable milestone:** end of P7 — DPV working across TFT + WiFi web + USB serial simultaneously.

**Build status (P4):** 1816/1816 objects, 623 KB binary, 60% flash free, 0 errors.

---

## P4 On-device UI — Dev Loop (LVGL PC Simulator)

All screen layout and animation work happens on the PC **before** flashing — rebuild takes < 2 s vs a 30–60 s flash cycle.

```
sim/
  lv_conf.h    — LVGL9 config matching firmware (depth 16, Montserrat 14/20/28)
  main_sim.c   — SDL2 sim entry point with synthetic DPV Gaussian scan
  README.md    — Windows setup instructions
```

See [`sim/README.md`](sim/README.md) for the full setup guide (`lv_port_pc_vscode` + SDL2 + CMake).

Quick start:
```
git clone --recursive https://github.com/lvgl/lv_port_pc_vscode
# copy sim/lv_conf.h and sim/main_sim.c into the clone
cmake -B build -G Ninja
cmake --build build
./build/lvgl_sdl          # opens 320×240 landscape window, full UI with synthetic scan
```

**What the sim proves:** screen layout, navigation flow, chart animation, transitions, fonts, colours.
**What only the real board proves:** RGB565 byte-swap colour accuracy, 40 MHz SPI refresh smoothness,
RAM budget under WiFi + LVGL simultaneously, physical button ergonomics.

---

## P1 Bench Bringup Checklist

Flash with `CONFIG_UBIOPOT_SELFTEST_MODE=y` (menuconfig → uBIOPOT → Dev/Debug):

    idf.py menuconfig          # enable UBIOPOT_SELFTEST_MODE
    idf.py -p <PORT> flash monitor

Selftest sequence (automated, ~40 s):
1. **LED blink** — READY + PROCESSING alternate × 3 (visual)
2. **DAC ramp** — steps 0→4095 in 512 increments, prints `Vout_expected` — verify with DMM at MCP4921 Vout pin
3. **ADC reads** — 10 samples from AIN1 (current) + AIN0 (voltage), logs µA and V
4. **Mux cycle** — T1→T2→T3→off, 100 ms each — verify with oscilloscope (only one HIGH at a time)
5. **Button wait** — 10 s window; press START (GPIO14) or NAV (GPIO0) to log debounce events

**P1 DoD** (hardware bench):
- [ ] `i2c_master_probe(0x48)` → `ADS1115 ADC init OK` in serial log
- [ ] DAC output linear: code 0→4095 tracks Vout 0→3.3 V (DMM)
- [ ] ADS1115 reads a known reference voltage correctly
- [ ] Mux lines isolate: only one of T1/T2/T3 HIGH at a time (scope)
- [ ] START + NAV buttons log single-click and long-press events
- [ ] READY LED = GPIO12 boots clean (no strapping fault)

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
