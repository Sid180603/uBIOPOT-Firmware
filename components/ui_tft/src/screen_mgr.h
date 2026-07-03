/**
 * @file screen_mgr.h
 * @brief Internal screen manager — LVGL screen creation, transitions, theme.
 *
 * This header is PRIVATE to the ui_tft component (PRIV_INCLUDE_DIRS "src").
 * External code uses ui_tft.h only.
 *
 * Each screen module exposes two functions:
 *   scr_xxx_create(lv_group_t *group) → creates and returns the lv_obj_t* screen.
 *   scr_xxx_xxx()                     → update functions called from the sink or engine.
 *
 * screen_mgr_init() creates all screens once. screen_mgr_goto_xxx() loads the
 * appropriate screen with lv_scr_load_anim().
 */

#pragma once

#include "lvgl.h"
#include "echem_core/scan_state.h"
#include "echem_core/peaks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Colour palette (RGB888 hex → lv_color_hex())
 * ------------------------------------------------------------------------- */
#define UI_COLOR_BG        0x0D0D0FU  /* near-black background                */
#define UI_COLOR_SURFACE   0x16213EU  /* dark-blue surface / card bg           */
#define UI_COLOR_ACCENT    0x00BCD4U  /* teal/cyan — live data trace           */
#define UI_COLOR_READY     0x4CAF50U  /* green — READY state                   */
#define UI_COLOR_PROC      0xFF9800U  /* amber — PROCESSING state              */
#define UI_COLOR_ABORT     0xF44336U  /* red — abort/error                     */
#define UI_COLOR_TEXT      0xE0E0E0U  /* near-white — primary text             */
#define UI_COLOR_DIM       0x757575U  /* grey — dimmed / secondary text        */
#define UI_COLOR_BORDER    0x2D2D4EU  /* subtle border / divider               */
#define UI_COLOR_FOCUS     0x1A6E8EU  /* darker teal — focused item bg         */

/* DPV scan chart: maximum points from engine (1 electrode) = 401; all-3 = 1203 */
#define UI_CHART_MAX_PTS   1300

/* Ring buffer for batching incoming DataPoints before LVGL frame flush */
#define UI_RING_BUF_SIZE   64

/* -------------------------------------------------------------------------
 * Screen manager lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief  Initialise the screen manager: apply theme, create all screens.
 *         Must be called inside lvgl_port_lock() after lvgl_port_add_disp().
 * @param  disp   LVGL display handle returned by lvgl_port_add_disp().
 * @param  indev  Encoder input device handle (returned by lv_indev_create).
 *                screen_mgr creates one lv_group per screen and switches the
 *                indev's active group on every screen transition so encoder
 *                focus always targets an object on the visible screen.
 */
void screen_mgr_init(lv_display_t *disp, lv_indev_t *indev);

/* -------------------------------------------------------------------------
 * Screen transitions — MUST be called under lvgl_port_lock().
 * They do NOT acquire the lock internally (avoids double-locking from the
 * engine sink or LVGL event callbacks, both of which hold the lock already).
 * ------------------------------------------------------------------------- */

void screen_mgr_goto_home(void);
void screen_mgr_goto_scan(void);
void screen_mgr_goto_results(void);
void screen_mgr_goto_settings(void);

/**
 * @brief  Toast severity level.
 *         INFO  — informational (teal accent strip, surface bg).
 *         ERROR — error / failure (red background).
 */
typedef enum {
    TOAST_INFO,
    TOAST_ERROR,
} toast_level_t;

/**
 * @brief  Show the toast overlay (slides in from top, auto-dismisses after 3 s).
 * @param  msg    Message string (must remain valid for at least 3 s).
 * @param  level  TOAST_INFO for informational messages, TOAST_ERROR for failures.
 */
void screen_mgr_show_toast(const char *msg, toast_level_t level);

/* -------------------------------------------------------------------------
 * Scan-live screen updates (called from the engine sink, already under lock)
 * ------------------------------------------------------------------------- */

/** Clear the scan chart and reset live readouts. Called at scan_started. */
void scr_scan_reset(uint8_t electrode);

/**
 * @brief  Add a DataPoint to the ring buffer.
 *         Thread-safe — can be called from Dispatcher (Core 0) context.
 *         The ring buffer is flushed to the LVGL chart by the per-frame timer.
 */
void scr_scan_push_point(float E_mV, float I_uA);

/** Update step progress display (n / total). */
void scr_scan_set_progress(uint16_t step, uint16_t total);

/** Switch scan screen to equilibration state (spinner + "Equilibrating..."). */
void scr_scan_set_equilibrating(bool eq);

/** Stop the elapsed time ticker (call when scan completes or aborts). */
void scr_scan_stop_elapsed(void);

/**
 * @brief  Return the electrode currently selected on the home screen.
 *         1/2/3 = individual electrode; 0 = All (sequential).
 *         Used by the engine sink to synchronise s_electrode on SCAN_EVT_START.
 */
uint8_t scr_home_get_electrode(void);

/* -------------------------------------------------------------------------
 * Results screen updates
 * ------------------------------------------------------------------------- */

/**
 * @brief  Populate the results screen with detected peaks.
 * @param  peaks      Array of peaks from peaks_find().
 * @param  n_peaks    Number of valid entries.
 * @param  electrode  Which electrode the scan was for (1/2/3; 0 = sequential all).
 */
void scr_results_set(const peak_t *peaks, uint16_t n_peaks, uint8_t electrode);

/**
 * @brief  Pass the raw scan curve to the results screen for the mini voltammogram.
 *         Call after scr_results_set(), before screen_mgr_goto_results().
 * @param  E_mV  Array of potential values (mV), length n.
 * @param  I_uA  Array of differential-current values (µA), same length.
 * @param  n     Number of points.
 */
void scr_results_set_curve(const float *E_mV, const float *I_uA, uint16_t n);

/* -------------------------------------------------------------------------
 * Settings screen: update WiFi info (called from P5 net_comms)
 * ------------------------------------------------------------------------- */
void scr_settings_set_wifi(const char *ssid, const char *ip, const char *url);

/* -------------------------------------------------------------------------
 * Home screen: update status fields
 * ------------------------------------------------------------------------- */
void scr_home_set_state(scan_state_t state);
void scr_home_set_electrode(uint8_t electrode);  /* 0 = All */

#ifdef __cplusplus
}
#endif
