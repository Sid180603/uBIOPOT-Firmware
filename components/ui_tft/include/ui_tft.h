#pragma once

/**
 * @file ui_tft.h
 * @brief On-device LVGL TFT UI public API.
 *
 * TODO P4: implement the following.
 *
 * Display: ILI9341 240x320, landscape (320x240), SPI2_HOST (HSPI).
 * Pins: SCLK=15, MOSI=2, CS=5, DC=4, RST=-1 (SW reset), MISO=-1, BL=hardwired 3V3.
 *
 * Memory: LVGL partial-render with 2 × ~19 KB DMA line buffers (NOT a 150 KB full framebuffer).
 * Input: custom LV_INDEV_TYPE_ENCODER; GPIO14=enter/start, GPIO0=rotate/navigate.
 *
 * Screens: Splash → Home/Menu → Scan-live (live chart) → Results → Settings/QR → Toast
 * Theme: dark near-black background, teal/cyan accent, Montserrat 14/20/28.
 *
 * UI is an engine sink (registers via engine_register_sink).
 * on_point  → ring buffer, batch-flushed once per LVGL frame under lvgl_port_lock.
 * on_event  → screen transitions.
 * on_resync → rebuild chart from server-auth buffer.
 */

/* TODO P4: ui_tft_start() */
