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
 * The synthetic DPV scan generates a four-metal voltammogram (Cd/Pb/Cu/Hg)
 * and routes it through the real peaks_find() + metal_identify() pipeline,
 * exercising the same detection + results-screen code as the device.
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
 * Synthetic DPV scan — four-metal voltammogram (Cd/Pb/Cu/Hg).
 *
 * Each metal is modelled as a Gaussian centred at its stripping potential:
 *   I(E) = Σ A_k · exp(−(E − µ_k)² / (2σ²))
 *
 * Centers and amplitudes (σ = 70 mV throughout):
 *   Cd  µ = −700 mV  A = 30 µA   window [−800, −600]
 *   Pb  µ = −450 mV  A = 50 µA   window [−550, −400]  ← tallest / primary
 *   Cu  µ =    0 mV  A = 25 µA   window [−100,  +50]
 *   Hg  µ =  +300 mV  A = 18 µA   window [+200, +400]
 *
 * Peaks are 250–450 mV apart; each drops to < 1% of its height before the
 * next begins — all four are cleanly separated for peaks_find().
 *
 * At scan end the buffer is passed to the *real* peaks_find() (threshold
 * 5.0 µA gives 3.6× margin above the smallest real peak while rejecting
 * noise when SIM_ADD_NOISE is enabled).
 *
 * Points emitted at 200 ms intervals → full scan in ~40 s sim-time.
 * ========================================================================= */

/* Set to 1 to add ±2 µA white noise (stress-tests the peak finder). */
#define SIM_ADD_NOISE 0

/* Four-metal table: { centre_mV, amplitude_uA } */
static const float k_sim_mu[4]  = { -700.0f, -450.0f,   0.0f, +300.0f };
static const float k_sim_amp[4] = {   30.0f,   50.0f,  25.0f,   18.0f };
#define SIM_SIGMA 70.0f

static int         s_step       = 0;
static bool        s_scanning   = false;
static lv_timer_t *s_scan_timer = NULL;

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

        /* Run the real peak finder — same code path the device uses. */
        static peak_t peaks[4];
        uint16_t n_pk = peaks_find(s_I_buf, s_E_buf, SIM_SCAN_POINTS,
                                   peaks, 4, 5.0f /* µA prominence */);
        scr_results_set(peaks, n_pk, /*electrode=*/1);
        scr_results_set_curve(s_E_buf, s_I_buf, (uint16_t)SIM_SCAN_POINTS);
        screen_mgr_goto_results();
        return;
    }

    float E = -1000.0f + (float)(s_step * 10);  /* −1000 → +990 mV */

    /* Sum of four Gaussians */
    float I = 0.0f;
    for (int k = 0; k < 4; k++) {
        float d = (E - k_sim_mu[k]) / SIM_SIGMA;
        I += k_sim_amp[k] * expf(-0.5f * d * d);
    }
#if SIM_ADD_NOISE
    I += 2.0f * ((float)rand() / (float)RAND_MAX - 0.5f) * 2.0f;  /* ±2 µA */
#endif

    s_E_buf[s_step] = E;
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
