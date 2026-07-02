#pragma once

/**
 * @file ui_tft.h
 * @brief On-device LVGL TFT UI — public API.
 *
 * Display: ILI9341 240×320, landscape (320×240), SPI2_HOST (HSPI).
 * Pins: SCLK=15, MOSI=2, CS=5, DC=4, RST=-1 (SW reset), MISO=-1, BL=hardwired 3V3.
 *
 * Memory: LVGL partial-render with 2 × ~19 KB DMA line buffers (NOT a 150 KB full framebuffer).
 * Input: custom LV_INDEV_TYPE_ENCODER; GPIO14=enter/start, GPIO0=rotate/navigate.
 *
 * Screens: Splash → Home/Menu → Scan-live (live chart) → Results → Settings/QR → Toast
 * Theme: dark near-black background, teal/cyan accent, Montserrat 14/20/28.
 *
 * UI registers as an engine sink after engine_init(). All sink callbacks run in
 * DispatcherTask (Core 0) — every LVGL call is wrapped in lvgl_port_lock().
 *
 * Encoder:
 *   GPIO0 PRESS_DOWN → enc_diff++  (navigate next)  → lvgl_port_task_wake
 *   GPIO14 PRESS_DOWN → enter pressed                → lvgl_port_task_wake
 *   GPIO14 PRESS_UP   → enter released               → lvgl_port_task_wake
 *   GPIO14 LONG_PRESS → abort running scan           → engine_abort()
 *   GPIO0  LONG_HOLD 10s → factory reset WiFi        → NVS erase (P8)
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise and start the on-device LVGL TFT UI.
 *
 * Performs in order:
 *   1. Initialise SPI2_HOST bus (SCLK=15, MOSI=2) for the ILI9341.
 *   2. Create the esp_lcd_panel_io + ILI9341 panel (CS=5, DC=4, 40 MHz, SW reset).
 *   3. Initialise LVGL via lvgl_port (task pinned to Core 0, 5 ms tick).
 *   4. Create the LVGL display (2×19 KB DMA partial-render buffers, swap_bytes, landscape).
 *   5. Set up the 2-button custom encoder indev.
 *   6. Create all screens (Splash/Home/Scan/Results/Settings/Toast).
 *   7. Register as an engine sink (on_point / on_event / on_resync).
 *   8. Show the Splash screen → auto-transition to Home after ~2 s.
 *
 * Must be called AFTER pstat_hal_init_all() and acq_engine_init().
 *
 * @return ESP_OK on success; ESP_FAIL or component error codes on failure.
 */
esp_err_t ui_tft_start(void);

/**
 * @brief  Update the WiFi information shown on the Settings screen.
 *
 * Called by net_comms (P5) once WiFi is configured.
 * Thread-safe: acquires lvgl_port_lock internally.
 *
 * @param ssid   Access-point SSID string (copied).
 * @param ip     IP address string, e.g. "192.168.4.1" (copied).
 * @param url    Full URL string, e.g. "http://ubiopot.local" (copied).
 */
void ui_tft_set_wifi_info(const char *ssid, const char *ip, const char *url);

#ifdef __cplusplus
}
#endif
