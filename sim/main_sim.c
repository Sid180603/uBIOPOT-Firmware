/**
 * @file main_sim.c
 * @brief Aqua-HMET PC Simulator entry point for lv_port_pc_vscode.
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
#include "pstat_hal/pstat_hal.h"  /* for esp_err_t pstat_led_set(pstat_led_t, bool) */

/* =========================================================================
 * Synthetic DPV scan — Pb²⁺ peak at −400 mV, σ = 80 mV, ΔI_peak = 50 µA.
 * Same Gaussian model used in host-test pyramid L3 / L7 (Wokwi cell sim).
 * Points emitted at 200 ms intervals → full scan in ~40 s sim-time.
 * ========================================================================= */

static int         s_step       = 0;
static bool        s_scanning   = false;
static lv_timer_t *s_scan_timer = NULL;

/* ISSUE 1: store scan data so scr_results_set_curve() can draw mini voltammogram */
#define SIM_SCAN_POINTS 200
static float s_E_buf[SIM_SCAN_POINTS];
static float s_I_buf[SIM_SCAN_POINTS];

static void synthetic_scan_cb(lv_timer_t *t)
{
    (void)t;
    if (s_step >= SIM_SCAN_POINTS) {
        lv_timer_del(s_scan_timer);
        s_scan_timer = NULL;
        s_scanning   = false;

        /* Announce results — Pb²⁺ peak at −400 mV / 50 µA on electrode 1 */
        static peak_t peaks[1] = {{ .E_mV = -400.0f, .I_uA = 50.0f, .index = 80 }};
        scr_results_set(peaks, 1, /*electrode=*/1);
        /* ISSUE 1 fix: supply raw curve data for the mini voltammogram */
        scr_results_set_curve(s_E_buf, s_I_buf, (uint16_t)SIM_SCAN_POINTS);
        screen_mgr_goto_results();
        return;
    }

    float E = -1000.0f + (float)(s_step * 10);      /* −1000 → +990 mV      */
    float I = 50.0f * expf(-(E + 400.0f) * (E + 400.0f)
                           / (2.0f * 80.0f * 80.0f)); /* Gaussian at −400 mV */

    s_E_buf[s_step] = E;   /* accumulate for set_curve */
    s_I_buf[s_step] = I;

    scr_scan_push_point(E, I);
    scr_scan_set_progress((uint16_t)(s_step + 1), SIM_SCAN_POINTS);
    s_step++;
}

/* =========================================================================
 * Engine + pstat_hal stubs
 * These replace the firmware's real implementations for the PC build.
 * The linker resolves the same symbols that the screen code calls.
 * ========================================================================= */

esp_err_t engine_start(uint8_t electrode, const dpv_params_t *params)
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

/* LED stub — no GPIO on PC. Signature matches real pstat_hal declaration. */
esp_err_t pstat_led_set(pstat_led_t led, bool on) { (void)led; (void)on; return 0; }

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
     *   lv_sdl_mousewheel_create()  — LV_INDEV_TYPE_ENCODER  <- we want this one
     *   lv_sdl_keyboard_create()    — LV_INDEV_TYPE_KEYPAD
     * All three are bound to lv_group_get_default().
     */
    lv_display_t *disp = sdl_hal_init(320, 240);

    /* ---- 3. Find the mousewheel encoder indev created by sdl_hal_init ----
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

    /* ---- 4. Init all screens and show splash ---- */
    screen_mgr_init(disp, enc_indev);

    printf("[SIM] Aqua-HMET PC Simulator ready\n");
    printf("[SIM] Mouse wheel = navigate   Middle click = select\n");
    printf("[SIM] Splash -> Home after ~2 s. Scroll to 'Start DPV' and middle-click.\n");

    /* ---- 5. Event loop — identical to original src/main.c ---- */
    while (1) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }

    return 0;
}
