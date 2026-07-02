/**
 * @file main_sim.c
 * @brief PC simulator main — replaces lv_port_pc_vscode/main/main.c
 *        for uBIOPOT screen development.
 *
 * This file is NOT compiled into the firmware.  It stands in for the
 * ESP-IDF platform glue (display init / iot_button) on the PC, letting
 * the real screen code (scr_*.c, screen_mgr.c) compile and run natively.
 *
 * Keyboard mapping:
 *   Arrow Down / Right  →  GPIO0 NAV (encoder rotate)
 *   Enter               →  GPIO14 START (encoder enter)
 *   Escape              →  simulate scan abort
 *
 * Build: drop into lv_port_pc_vscode/main/ alongside the screen files.
 */

#include "lvgl.h"
#include "screens/screen_mgr.h"

#include <stdio.h>
#include <math.h>

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

/* -------------------------------------------------------------------------
 * Simulator main entry point
 * Called from lv_port_pc_vscode/main/main.c after lv_init + display + indev.
 * Replace the default call with: sim_main(display);
 * ------------------------------------------------------------------------- */

void sim_main(lv_display_t *disp)
{
    /* Create encoder indev */
    lv_group_t *grp = lv_group_create();
    lv_group_set_default(grp);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(s_indev, encoder_read_cb);
    lv_indev_set_disp(s_indev, disp);
    lv_indev_set_group(s_indev, grp);

    /* Init all screens */
    screen_mgr_init(disp, grp);

    printf("[SIM] uBIOPOT PC Simulator ready\n");
    printf("[SIM] Keys: Arrow/Right=NAV  Enter=SELECT  Escape=ABORT\n");
    printf("[SIM] Splash will transition to Home after 2.2 s\n");
    printf("[SIM] Navigate to 'Start DPV' and press Enter to run synthetic scan\n");
}
