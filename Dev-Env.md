

Fetched 2 resources

There are **4 levels** of dev environments you can use right now, all without touching the board:

---

## Level 1 — Node.js Mock Backend (fastest, 2 commands)

This is the most immediately useful. `mock_backend.js` serves the SPA **and** provides a live `/ws` endpoint that speaks the exact same binary protocol as the real firmware, generating synthetic Gaussian DPV peaks (Cd²⁺ at −700 mV, Pb²⁺ at −400 mV, Cu²⁺ at 0 mV).

```bash
# In WSL (Node.js via nvm, or install it):
node --version 2>/dev/null || curl -fsSL https://deb.nodesource.com/setup_20.x | bash - && apt-get install -y nodejs

cd '/c/Users/z00541ce/Documents/Coding/ST Thesis/uBIOPOT-Firmware/web'
node mock_backend.js --port 3000
```

Then open `http://localhost:3000` in your browser on Windows. You'll see the real SPA. Click **Start DPV** → the voltammogram draws live with three Gaussian peaks. Every UI feature works: abort, CSV export, reference import, x-axis toggle.

**What this proves:** P6 SPA is fully functional. WS binary protocol is correctly parsed. All UI flows work.

---

## Level 2 — Playwright E2E Tests (automated, headless)

24 automated tests that verify specific behaviours without you clicking anything:

```bash
cd '/c/Users/z00541ce/Documents/Coding/ST Thesis/uBIOPOT-Firmware/web'
npm install
npx playwright install chromium --with-deps
npm test
```

**What this proves:** Every contract (WS connects → hello received → Start enabled → binary frames → pts counter updates → scan_complete → CSV button enabled → download triggered → abort stops scan, etc.) is verified automatically in ~30 seconds.

---

## Level 3 — Python pytest (already passing from P5)

```bash
cd '/c/Users/z00541ce/Documents/Coding/ST Thesis/uBIOPOT-Firmware'
pytest test/test_p5_protocol.py -v -k "not device"
```

**What this proves:** The WS wire protocol itself — binary frame encoding, JSON event schema, CSV format, mock server fanout. No browser, no device.

---

## Level 4 — Wokwi (real firmware, no physical board)

Wokwi runs **the actual compiled ESP-IDF firmware** in a web browser — not a simulator of the application, but the real binary. WiFi, SPI, I2C, FreeRTOS all simulated. This is the plan's L7 layer.

**How for Aqua-HMET:**

```bash
# Create merged flash image (bootloader + pt + app + littlefs)
cd '/c/Users/z00541ce/Documents/Coding/ST Thesis/uBIOPOT-Firmware'
source /home/Sid18/esp/esp-idf/export.sh
idf.py uf2   # creates build/uf2.bin
```

Then at [wokwi.com](https://wokwi.com/projects/new/esp32):
1. Press `F1` → **"Upload Firmware and Start Simulation"**
2. Upload `build/uf2.bin`
3. The firmware boots, LVGL renders on the simulated ILI9341 panel, WiFi SoftAP starts
4. Wokwi has a simulated WiFi AP you can "join" from the sim's browser to hit the HTTP/WS server

**Limitation for Aqua-HMET:** The ADS1115 (I2C) and MCP4921 (SPI) would need custom Wokwi chips (C/WASM) to close the analog loop — without them the HAL self-test would fail but the TFT UI and WiFi server would still run. Custom chips are the plan's L7 "centerpiece" and are a future CI task.
