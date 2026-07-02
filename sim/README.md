# uBIOPOT LVGL PC Simulator

Develop and iterate on the TFT UI screens on your laptop using SDL2 — **no ESP32 needed, no flash cycles**.

## Why this works

LVGL is platform-agnostic. It draws pixels into a buffer and calls two hooks:
- `flush_cb(area, pixels)` → on device: SPI to ILI9341; on PC: SDL blits to a 320×240 window
- `indev_read_cb(data)` → on device: GPIO14/0; on PC: keyboard (Arrow keys + Enter)

Your actual screen code (`scr_splash.c`, `scr_home.c`, etc.) **compiles identically on both**. Only the platform glue differs.

## Setup (Windows)

### Prerequisites

```powershell
# Option A — vcpkg
vcpkg install sdl2:x64-windows

# Option B — download SDL2 mingw64 from https://libsdl.org/download-2.0.php
# Extract and note the path
```

Also install: CMake, Ninja, and a C compiler (MinGW or MSVC).

### Clone the base simulator

```powershell
cd sim\
git clone --recursive https://github.com/lvgl/lv_port_pc_vscode lv_port_pc_vscode
cd lv_port_pc_vscode
```

### Configure for uBIOPOT

1. **Copy `lv_conf.h`** (from this `sim/` directory) into the cloned `lv_port_pc_vscode/` root.

2. **Copy the screen sources** from `../components/ui_tft/src/` into `lv_port_pc_vscode/src/screens/`:
   ```
   scr_splash.c  scr_home.c  scr_scan.c  scr_results.c  scr_settings.c  scr_toast.c  screen_mgr.c  screen_mgr.h
   ```

3. **Edit `lv_port_pc_vscode/main/main.c`** — replace the demo call with:
   ```c
   #include "screens/screen_mgr.h"

   // Provide stub includes (see main_sim.c in this directory)
   extern void sim_init_screens(lv_display_t *disp);

   // In your main loop init:
   sim_init_screens(display);
   ```
   Or use the provided `main_sim.c` as a drop-in replacement.

4. **Build and run**:
   ```powershell
   mkdir build
   cmake -B build -G Ninja -DSDL2_DIR=<path_to_SDL2_cmake>
   cmake --build build
   .\build\lvgl_sdl.exe
   ```

   VS Code: Press **F5** (the project already has a launch.json).

## Keyboard controls (PC sim → encoder)

| Key       | Maps to           |
|-----------|-------------------|
| Arrow Down / Right | GPIO0 NAV (cycle focus) |
| Enter     | GPIO14 START (select/confirm) |
| Escape    | GPIO14 long-press (abort scan) |

## Simulated scan data

`main_sim.c` includes a synthetic DPV Gaussian-peak model (same as test pyramid L3/L7):

```c
// Pb²⁺ peak at -400 mV, σ=80 mV, max ΔI=50 µA
static void synthetic_timer_cb(lv_timer_t *t) {
    float E = -1000.0f + (step_idx * 10.0f);
    float I = 50.0f * expf(-(E + 400.0f)*(E + 400.0f) / (2.0f * 80.0f * 80.0f));
    scr_scan_push_point(E, I);
    scr_scan_set_progress(++step_idx, 200);
    if (step_idx >= 200) lv_timer_del(t);
}
```

The live chart will draw left→right, the PROCESSING dot pulses, then transitions to Results.

## What to iterate on here (before flashing)

- Layout / spacing / font sizes — change and rebuild in < 2 seconds
- Colour palette (`UI_COLOR_*` in `screen_mgr.h`)
- Animation timings (slide ms, pulse period)
- Menu item labels and ordering
- Chart axis ranges and grid density
- Results screen peak display format

## What you STILL need to flash for

- Actual colour accuracy (ILI9341 byte-swap / BGR vs SDL)
- Refresh smoothness at 40 MHz SPI under WiFi + acquisition load
- RAM budget ("fits in ~320 KB alongside WiFi+LVGL" — only provable on chip)
- Physical button feel (GPIO14/0 short vs long press)
- Panel readability at arm's length on 2.4"

**Typical flow: iterate visuals on PC (30+ times per session), flash once to verify.**
