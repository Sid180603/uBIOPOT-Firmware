/**
 * @file main_sim.c
 * @brief uBIOPOT PC Simulator entry point for lv_port_pc_vscode.
 *
 * Replaces src/main.c (which called lv_demo_widgets()).
 *
 * Structure mirrors the original main.c EXACTLY — only the demo call
 * is replaced with our screen initialisation:
 *
 *   lv_init()
 *   sdl_hal_init(320, 240)     <- HAL creates display + mouse/wheel/keyboard indevs
 *   screen_mgr_init(disp, enc) <- init screens; enc = mousewheel encoder from HAL
 *   while(1) lv_timer_handler() + usleep()   <- unchanged event loop
 *
 * SDL events (mouse, keyboard, window close) are handled INTERNALLY by
 * LVGL's SDL driver during lv_timer_handler().  No SDL_PollEvent needed here.
 *
 * Navigation in the sim window:
 *   Mouse wheel scroll   →  navigate focus (encoder rotate)
 *   Middle mouse button  →  select / confirm (encoder press)
 *   Keyboard Tab / ↓ / → →  navigate (via lv_sdl_keyboard indev)
 *   Keyboard Enter       →  select
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* needed for usleep() */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include "lvgl/lvgl.h"
#include "hal/hal.h"
#include "screens/screen_mgr.h"
#include "echem_core/scan_state.h"
#include "echem_core/dpv.h"
#include "echem_core/peaks.h"

/* =========================================================================
 * Synthetic DPV scan — Pb²⁺ peak at −400 mV, σ = 80 mV, ΔI_peak = 50 µA.
 * Same Gaussian model used in host-test pyramid L3 / L7 (Wokwi cell sim).
 * Points emitted at 200 ms intervals → full scan in ~40 s sim-time.
 * ========================================================================= */

static int         s_step       = 0;
static bool        s_scanning   = false;
static lv_timer_t *s_scan_timer = NULL;

static void synthetic_scan_cb(lv_timer_t *t)
{
    (void)t;
    if (s_step >= 200) {
        lv_timer_del(s_scan_timer);
        s_scan_timer = NULL;
        s_scanning   = false;

        /* Announce results — Pb²⁺ peak at −400 mV / 50 µA on electrode 1 */
        static peak_t peaks[1] = {{ .E_mV = -400.0f, .I_uA = 50.0f, .index = 80 }};
        scr_results_set(peaks, 1, /*electrode=*/1);
        screen_mgr_goto_results();
        return;
    }

    float E = -1000.0f + (float)(s_step * 10);      /* −1000 → +990 mV      */
    float I = 50.0f * expf(-(E + 400.0f) * (E + 400.0f)
                           / (2.0f * 80.0f * 80.0f)); /* Gaussian at −400 mV */

    scr_scan_push_point(E, I);
    scr_scan_set_progress((uint16_t)(s_step + 1), 200);
    s_step++;
}

/* =========================================================================
 * Engine + pstat_hal stubs
 * These replace the firmware's real implementations for the PC build.
 * The linker resolves the same symbols that the screen code calls.
 * ========================================================================= */

int engine_start(uint8_t electrode, const dpv_params_t *params)
{
    (void)params;
    if (s_scanning) return -1;

    s_step     = 0;
    s_scanning = true;

    scr_scan_reset(electrode);
    scr_scan_set_equilibrating(false);   /* equilibration stubbed to instant */
    screen_mgr_goto_scan();

    /* One data point every 200 ms — slow enough to watch the chart draw */
    s_scan_timer = lv_timer_create(synthetic_scan_cb, 200, NULL);
    return 0; /* ESP_OK */
}

void engine_abort(void)
{
    if (s_scan_timer) {
        lv_timer_del(s_scan_timer);
        s_scan_timer = NULL;
    }
    s_scanning = false;
    printf("[SIM] engine_abort\n");
    screen_mgr_goto_home();
}

scan_state_t engine_get_state(void)
{
    return s_scanning ? SCAN_STATE_RUNNING : SCAN_STATE_IDLE;
}

/* LED stub — no GPIO on PC */
void pstat_led_set(int led, int on) { (void)led; (void)on; }

/* =========================================================================
 * main — mirrors original src/main.c structure exactly.
 * Only change: replace lv_demo_widgets() with our screen initialisation.
 * ========================================================================= */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- 1. Init LVGL ---- */
    lv_init();

    /* ---- 2. Init HAL (SDL window + all indevs) ----
     * sdl_hal_init creates internally:
     *   lv_sdl_mouse_create()       — LV_INDEV_TYPE_POINTER
     *   lv_sdl_mousewheel_create()  — LV_INDEV_TYPE_ENCODER  ← we want this one
     *   lv_sdl_keyboard_create()    — LV_INDEV_TYPE_KEYPAD
     * All three are bound to lv_group_get_default().
     */
    lv_display_t *disp = sdl_hal_init(320, 240);

    /* ---- 3. Find the mousewheel encoder indev ----
     * screen_mgr needs it to swap the focused group on each screen transition,
     * ensuring the wheel navigates the items on the currently visible screen.
     */
    lv_indev_t *enc_indev = NULL;
    for (lv_indev_t *iv = lv_indev_get_next(NULL); iv != NULL;
         iv = lv_indev_get_next(iv)) {
        if (lv_indev_get_type(iv) == LV_INDEV_TYPE_ENCODER) {
            enc_indev = iv;
            break;
        }
    }

    /* ---- 4. Init screens and show splash ---- */
    screen_mgr_init(disp, enc_indev);

    printf("[SIM] uBIOPOT PC Simulator ready\n");
    printf("[SIM] Mouse wheel = navigate   Middle click = select\n");
    printf("[SIM] Splash → Home after ~2 s. Navigate to 'Start DPV' and middle-click.\n");

    /* ---- 5. Event loop — identical to original main.c ---- */
    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }

    return 0;
}


/* -------------------------------------------------------------------------
 * Synthetic DPV data — same Gaussian-peak model as test pyramid L3/L7.
 * Pb²⁺ peak at -400 mV, sigma=80 mV, ΔI_peak = 50 µA.
 * ------------------------------------------------------------------------- */

static int      s_step = 0;
static bool     s_scanning = false;
static lv_timer_t *s_scan_timer = NULL;

static void synthetic_scan_cb(lv_timer_t *t)
{
    (void)t;
    if (s_step >= 200) {
        lv_timer_del(s_scan_timer);
        s_scan_timer = NULL;
        s_scanning = false;
        /* Simulate scan complete → go to results */
        static peak_t peaks[1] = {{ .E_mV = -400.0f, .I_uA = 50.0f, .index = 80 }};
        scr_results_set(peaks, 1, 1);
        screen_mgr_goto_results();
        return;
    }

    float E = -1000.0f + (s_step * 10.0f);
    float I = 50.0f * expf(-(E + 400.0f) * (E + 400.0f) / (2.0f * 80.0f * 80.0f));

    scr_scan_push_point(E, I);
    scr_scan_set_progress((uint16_t)(s_step + 1), 200);
    s_step++;
}

/* -------------------------------------------------------------------------
 * Keyboard → encoder indev
 * ------------------------------------------------------------------------- */

static lv_indev_t *s_indev = NULL;
static volatile int32_t s_enc_diff = 0;
static volatile bool    s_enter    = false;

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = s_enc_diff;
    data->state    = s_enter ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_enc_diff     = 0;
}

/* -------------------------------------------------------------------------
 * SDL keyboard event handler
 * ------------------------------------------------------------------------- */

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t key = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_KEY) {
        /* Use lv_indev_get_key if needed */
    }
}

/* -------------------------------------------------------------------------
 * Stub functions (replace pstat_hal / acq_engine stubs for PC build)
 * These prevent linker errors when screen files include acq_engine.h.
 * In the firmware build, these are the real implementations.
 * ------------------------------------------------------------------------- */

/* engine_start: on PC, start synthetic scan */
int engine_start(uint8_t electrode, const void *params)
{
    (void)electrode; (void)params;
    if (s_scanning) return -1;
    s_step    = 0;
    s_scanning = true;
    scr_scan_reset(electrode);
    scr_scan_set_equilibrating(false);
    screen_mgr_goto_scan();
    /* Start at 5 points/second = 200 ms per point */
    s_scan_timer = lv_timer_create(synthetic_scan_cb, 200, NULL);
    return 0; /* ESP_OK */
}

void engine_abort(void)
{
    if (s_scan_timer) { lv_timer_del(s_scan_timer); s_scan_timer = NULL; }
    s_scanning = false;
    printf("[SIM] engine_abort called\n");
    screen_mgr_goto_home();
}

/* pstat_led_set stub */
void pstat_led_set(int led, int on) { (void)led; (void)on; }

/* engine_get_state stub */
scan_state_t engine_get_state(void)
{
    return s_scanning ? SCAN_STATE_RUNNING : SCAN_STATE_IDLE;
}

/* -------------------------------------------------------------------------
 * Simulator main entry point
 * Called from lv_port_pc_vscode/main/main.c after lv_init + display + indev.
 * Replace the default call with: sim_main(display);
 * ------------------------------------------------------------------------- */

void sim_main(lv_display_t *disp)
{
    /* Create encoder indev — screen_mgr_init (post-P4-review) takes the indev
     * directly and manages per-screen groups internally. */
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(s_indev, encoder_read_cb);
    lv_indev_set_disp(s_indev, disp);

    /* Init all screens — pass indev so screen_mgr can swap per-screen groups */
    screen_mgr_init(disp, s_indev);

    printf("[SIM] uBIOPOT PC Simulator ready\n");
    printf("[SIM] Keys: Arrow/Right=NAV  Enter=SELECT  Escape=ABORT\n");
    printf("[SIM] Splash will transition to Home after 2.2 s\n");
    printf("[SIM] Navigate to 'Start DPV' and press Enter to run synthetic scan\n");
}
