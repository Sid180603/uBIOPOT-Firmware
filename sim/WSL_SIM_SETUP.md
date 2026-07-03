# uBIOPOT LVGL Simulator — WSL Setup Guide

Develop and iterate on the TFT UI screens on your laptop using the LVGL PC simulator
(`lv_port_pc_vscode` + SDL2). Rebuild takes **< 2 s** vs a 30–60 s flash cycle.

**Verified on:** Windows 11 + WSL2 Debian 13 (trixie) + WSLg (built-in GUI).

---

## How it works

LVGL draws pixels into a buffer and calls two hooks:

- `flush_cb(area, pixels)` → on device: SPI to ILI9341; **on PC: SDL blits to a 320×240 window**
- `indev_read_cb(data)` → on device: GPIO14/GPIO0; **on PC: SDL mouse wheel + keyboard**

The screen source files (`scr_*.c`, `screen_mgr.c`) are **byte-identical** between the firmware
and the simulator. Only the platform glue differs.

---

## Prerequisites

### 1. WSL2 with WSLg (display support)

WSLg is included with Windows 11 and provides a native X display for Linux GUI apps.
Verify it works:

```bash
echo $DISPLAY   # should print :0
```

If `$DISPLAY` is empty, update Windows and WSL:
```powershell
wsl --update
```

### 2. Install build dependencies (WSL terminal)

```bash
sudo apt update
sudo apt install -y libsdl2-dev cmake ninja-build gcc g++
```

- `libsdl2-dev` — SDL2 (window, mouse, keyboard, display)
- `cmake` + `ninja-build` — build system
- `gcc` / `g++` — C/C++ compiler

> `libm-dev` does NOT exist on Debian — the math library is bundled with `libc6-dev`
> (already installed with gcc). Do not add it.

---

## Setup Steps

### Step 1 — Clone the base simulator

```bash
cd ~
git clone --recursive https://github.com/lvgl/lv_port_pc_vscode ubiopot_sim
```

This pulls LVGL9 and FreeRTOS as submodules (~700 MB total, takes ~2 minutes).

---

### Step 2 — Set variables

```bash
REPO="/c/Users/<your-username>/Documents/Coding/ST Thesis/uBIOPOT-Firmware"
SIM=~/ubiopot_sim
```

Replace `<your-username>` with your Windows username (e.g. `z00541ce`).

---

### Step 3 — Copy our LVGL config

```bash
cp "$REPO/sim/lv_conf.h" "$SIM/"
```

This replaces `lv_port_pc_vscode`'s default `lv_conf.h` with one that matches the firmware:
color depth 16, Montserrat 14/20/28, widgets we actually use, `LV_STDLIB_CLIB` (standard
`malloc` — correct for PC; the firmware uses `BUILTIN` for its bounded ESP32 heap).

---

### Step 4 — Copy the simulator entry point

```bash
cp "$REPO/sim/main_sim.c" "$SIM/src/main.c"
```

Replaces the default `lv_demo_widgets()` call with our screens + synthetic DPV scan.

---

### Step 5 — Copy screen source files

```bash
mkdir -p "$SIM/src/screens"
cp "$REPO/components/ui_tft/src/"scr_*.c   "$SIM/src/screens/"
cp "$REPO/components/ui_tft/src/screen_mgr.c" "$SIM/src/screens/"
cp "$REPO/components/ui_tft/src/screen_mgr.h" "$SIM/src/screens/"
```

These are the **exact same `.c` files** compiled into the firmware. No duplication — one source
of truth for both targets.

---

### Step 6 — Copy IDF stub headers

The screen files include `esp_log.h`, `acq_engine.h`, `freertos/FreeRTOS.h`, and
`esp_lvgl_port.h` — headers that only exist inside ESP-IDF. On the PC we provide lightweight
stubs that let the code compile without any IDF installation.

```bash
mkdir -p "$SIM/src/stubs/freertos"
mkdir -p "$SIM/src/stubs/echem_core"

# IDF-specific stubs
cp "$REPO/sim/stubs/esp_log.h"           "$SIM/src/stubs/"
cp "$REPO/sim/stubs/esp_err.h"           "$SIM/src/stubs/"
cp "$REPO/sim/stubs/acq_engine.h"        "$SIM/src/stubs/"
cp "$REPO/sim/stubs/esp_lvgl_port.h"     "$SIM/src/stubs/"
cp "$REPO/sim/stubs/freertos/FreeRTOS.h" "$SIM/src/stubs/freertos/"
cp "$REPO/sim/stubs/freertos/semphr.h"   "$SIM/src/stubs/freertos/"

# echem_core pure headers (no IDF deps — compile directly)
cp "$REPO/components/echem_core/include/echem_core/"*.h "$SIM/src/stubs/echem_core/"
```

| Stub | Replaces |
|------|----------|
| `esp_log.h` | `ESP_LOGI/W/E` → `printf` wrappers |
| `esp_err.h` | `esp_err_t = int`, `ESP_OK = 0` |
| `acq_engine.h` | Forward-declares `engine_start/abort/get_state` (implemented in `main_sim.c`) |
| `esp_lvgl_port.h` | `lvgl_port_lock/unlock` as no-op inlines |
| `freertos/*.h` | Mutex macros as no-ops (single-threaded SDL sim) |

---

### Step 7 — Patch `CMakeLists.txt`

Three edits are needed. Run this Python script once:

```bash
python3 << 'EOF'
import re

path = '/home/Sid18/ubiopot_sim/CMakeLists.txt'   # adjust username if needed
txt = open(path).read()

# 1. Replace the non-existent main/inc include with our actual paths
txt = txt.replace(
    'include_directories(${PROJECT_SOURCE_DIR}/main/inc)',
    'include_directories(${PROJECT_SOURCE_DIR}/src)\n'
    'include_directories(${PROJECT_SOURCE_DIR}/src/screens)\n'
    'include_directories(${PROJECT_SOURCE_DIR}/src/stubs)'
)

# 2. Glob and add screen sources right after MAIN_SOURCES is defined
txt = txt.replace(
    'set(MAIN_SOURCES src/mouse_cursor_icon.c src/hal/hal.c)',
    'set(MAIN_SOURCES src/mouse_cursor_icon.c src/hal/hal.c)\n'
    'file(GLOB SCREEN_SOURCES "${PROJECT_SOURCE_DIR}/src/screens/*.c")\n'
    'list(APPEND MAIN_SOURCES ${SCREEN_SOURCES})'
)

# 3. Disable LVGL examples + demos (they need 15+ widgets we don't enable)
#    and use only the lvgl core (not lvgl::examples/demos/thorvg)
txt = txt.replace(
    'add_subdirectory(lvgl)',
    '# Disable LVGL examples and demos — only core library needed\n'
    'set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)\n'
    'set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)\n'
    'add_subdirectory(lvgl)'
)
txt = txt.replace(
    'set(MAIN_LIBS lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES})',
    'set(MAIN_LIBS lvgl ${SDL2_LIBRARIES})'
)

# 4. Fix BUILD_INTERFACE (CMake 3.12+ policy for PUBLIC include dirs)
txt = txt.replace(
    'target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})',
    'target_include_directories(lvgl PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}> ${SDL2_INCLUDE_DIRS})'
)

open(path, 'w').write(txt)
print("CMakeLists.txt patched OK")
EOF
```

**What each patch does:**

| Patch | Why |
|-------|-----|
| Add `src/`, `src/screens/`, `src/stubs/` to include paths | Screen files use `#include "screen_mgr.h"` and `#include "esp_log.h"` — compiler needs to find them |
| Glob `src/screens/*.c` into `MAIN_SOURCES` | CMake only knew about `src/main.c` + HAL — all 7 screen files need to be compiled |
| `CONFIG_LV_BUILD_EXAMPLES/DEMOS=OFF` before `add_subdirectory(lvgl)` | Prevents LVGL from building 500+ example files that need widgets we've disabled |
| Remove `lvgl::examples lvgl::demos lvgl::thorvg` from `MAIN_LIBS` | We replaced `main.c` with our own so these targets are unreferenced dead weight |
| `$<BUILD_INTERFACE:...>` wrapper | CMake 3.12+ refuses raw absolute paths in PUBLIC include dirs of installed targets |

---

### Step 8 — Build

First build (compiles LVGL core ~400 files + our screens):

```bash
cd ~/ubiopot_sim
cmake -B build -G Ninja
cmake --build build -j$(nproc)
```

Expected output ends with:
```
[N/N] Linking CXX executable /home/.../ubiopot_sim/bin/main
```

No errors. If errors appear, see the Troubleshooting section below.

**Subsequent rebuilds** (after editing a screen file) — just re-copy and rebuild:

```bash
cp "$REPO/components/ui_tft/src/scr_scan.c" ~/ubiopot_sim/src/screens/
cmake --build build -j$(nproc)   # ~2 seconds
```

---

### Step 9 — Run

```bash
./bin/main
```

A **320×240 landscape window** appears on the Windows desktop (WSLg). Log output:

```
[I][scr_splash] Splash screen created
[I][scr_home] Home screen created
...
[SIM] uBIOPOT PC Simulator ready
[SIM] Mouse wheel = navigate   Middle click = select
[SIM] Splash -> Home after ~2 s. Scroll to 'Start DPV' and middle-click.
```

---

## Navigation

| Input | Action |
|-------|--------|
| **Mouse wheel scroll** | Move focus between menu items |
| **Middle mouse button** | Activate focused item (select) |
| **Tab** | Move focus forward |
| **Enter** | Activate focused item |
| **Two-finger scroll** (touchpad) | Same as mouse wheel |

---

## Simulated flow

1. **Splash** — "uBIOPOT v2" fades in
2. **Home** — menu list; teal highlight on focused item
3. Navigate to **Start DPV** → press Enter/middle-click
4. **Scan-Live** — voltammogram draws left→right (Pb²⁺ peak at −400 mV)
5. Scan completes → **Results** — peak −400 mV / 50 µA
6. Press **Enter** on "Run Again" → back to Home

---

## Iterating on screen design

Edit any screen file in `components/ui_tft/src/`, re-copy, and rebuild:

```bash
# Edit scr_scan.c in VS Code, then:
cp "$REPO/components/ui_tft/src/scr_scan.c" ~/ubiopot_sim/src/screens/
cmake --build ~/ubiopot_sim/build -j$(nproc) && ~/ubiopot_sim/bin/main
```

**Design on PC → verify on board.** The sim nails layout, animations, colours, and navigation
flow. Flash the board once to confirm RGB565 byte-swap colour accuracy, 40 MHz SPI refresh
smoothness, and GPIO button ergonomics.

---

## Troubleshooting

### `Cannot create regular file '.../main.c': No such file or directory`

The source directory is `src/`, not `main/`. Use `src/main.c` paths throughout.

### `CMake Error: INTERFACE_INCLUDE_DIRECTORIES ... prefixed in the source directory`

Apply patch 4 (the `$<BUILD_INTERFACE:...>` fix) and `rm -rf build` before re-running cmake.

### `FAILED: lvgl/CMakeFiles/lvgl_examples.dir/...` (slider/dropdown missing)

LVGL examples require widgets we've intentionally disabled. Apply patch 3
(`CONFIG_LV_BUILD_EXAMPLES=OFF`) and `rm -rf build`.

### `undefined reference to 'main'`

`main_sim.c` must contain `int main(int argc, char **argv)`. If missing, re-copy from the repo
(the file was truncated or an old version was used).

### Segfault in `lv_tlsf_create` / `lv_mem_init`

The `lv_conf.h` in `ubiopot_sim/` is the old version with `LV_STDLIB_BUILTIN` and a broken
`LV_MEM_POOL_ALLOC` empty macro. Re-copy `sim/lv_conf.h` from the repo — it uses
`LV_STDLIB_CLIB` (standard `malloc`, correct for desktop):

```bash
cp "$REPO/sim/lv_conf.h" ~/ubiopot_sim/lv_conf.h
cmake --build build -j$(nproc)
```

### Window does not appear / `Cannot connect to display`

Check `echo $DISPLAY` — should print `:0`. If empty, update WSL and Windows:
```powershell
wsl --update
```
Then restart WSL and try again.

### `Unable to locate package libm-dev`

This package does not exist on Debian. `libm` is bundled with `libc6-dev` (installed with `gcc`).
Remove `libm-dev` from the install command.
