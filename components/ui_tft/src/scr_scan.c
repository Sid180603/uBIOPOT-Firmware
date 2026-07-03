/**
 * @file scr_scan.c
 * @brief Scan-live screen — real-time DPV voltammogram.
 *
 * Layout (landscape 320×240):
 *   ┌──────────────────────────────────────────────────┐
 *   │ STATUS BAR  E1  DPV  ●●●  10/240  +0.120V        │  h=24
 *   ├──────────────────────────────────────────────────┤
 *   │                                                   │
 *   │   lv_chart scatter   dI (µA) vs E (V)            │  h=152
 *   │   (draws L→R as points arrive)                    │
 *   │                                                   │
 *   ├──────────────────────────────────────────────────┤
 *   │ dI:  +0.04 µA     E:  +0.120 V    ⊙ MEASURING    │  h=34
 *   ├──────────────────────────────────────────────────┤
 *   │  [NAV: nav]              [Hold START: abort]      │  h=30
 *   └──────────────────────────────────────────────────┘
 *
 * Data path (non-blocking):
 *   AcquisitionTask → DataPoint queue → DispatcherTask → scr_scan_push_point()
 *   → ring buffer → LVGL frame timer → lv_chart_set_next_value2()
 *
 * The ring buffer decouples the Dispatcher (Core 0) from LVGL rendering.
 * The per-frame timer flushes accumulated points in one batch, keeping
 * the voltammogram smooth without per-point stutter.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "acq_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "scr_scan";

/* -------------------------------------------------------------------------
 * Ring buffer — written by Dispatcher, flushed by LVGL frame timer
 * ------------------------------------------------------------------------- */

typedef struct {
    int32_t E_mV;   /* rounded to nearest mV */
    int32_t I_uA;   /* rounded to nearest µA */
} chart_point_t;

static chart_point_t s_ring[UI_RING_BUF_SIZE];
static volatile uint8_t s_ring_head = 0;  /* write index (Dispatcher) */
static volatile uint8_t s_ring_tail = 0;  /* read index  (LVGL timer)  */

/* -------------------------------------------------------------------------
 * Screen state
 * ------------------------------------------------------------------------- */

static lv_obj_t  *s_scr          = NULL;
static lv_obj_t  *s_lbl_elec     = NULL;  /* "E1  DPV" in status bar */
static lv_obj_t  *s_lbl_progress = NULL;  /* "10/240" step count */
static lv_obj_t  *s_lbl_elapsed  = NULL;  /* "M:SS" elapsed time (#13, replaces live V in bar) */
static lv_obj_t  *s_lbl_live_i   = NULL;  /* live current display */
static lv_obj_t  *s_lbl_proc     = NULL;  /* "⊙ MEASURING" / "Equilibrating..." */
static lv_obj_t  *s_chart        = NULL;
static lv_chart_series_t *s_series[3];    /* one per electrode */
static lv_obj_t  *s_spinner      = NULL;  /* equilibration spinner */
static lv_timer_t *s_flush_timer = NULL;  /* per-frame batch flush */
static lv_timer_t *s_elapsed_timer = NULL; /* 1-second elapsed time ticker (#13) */

/* Elapsed time counter (seconds since scan start) */
static uint32_t s_elapsed_s = 0;

/* Dynamic Y-axis range tracking */
static int32_t s_y_min = -100;
static int32_t s_y_max =  100;

/* -------------------------------------------------------------------------
 * Ring buffer API (lock-free single-producer/single-consumer)
 * ------------------------------------------------------------------------- */

static void ring_push(float E_mV, float I_uA)
{
    uint8_t next = (s_ring_head + 1) & (UI_RING_BUF_SIZE - 1);
    if (next == s_ring_tail) return;  /* full — drop (DPV rate << UI rate) */
    s_ring[s_ring_head].E_mV = (int32_t)roundf(E_mV);
    s_ring[s_ring_head].I_uA = (int32_t)roundf(I_uA);
    s_ring_head = next;
}

static bool ring_pop(chart_point_t *out)
{
    if (s_ring_tail == s_ring_head) return false;
    *out = s_ring[s_ring_tail];
    s_ring_tail = (s_ring_tail + 1) & (UI_RING_BUF_SIZE - 1);
    return true;
}

/* -------------------------------------------------------------------------
 * PULSING processing dot animation
 * ------------------------------------------------------------------------- */

static lv_anim_t s_proc_anim;

/* Wrapper: animates object opacity without needing a 3-arg cast */
static void anim_set_opa(void *obj, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void start_proc_anim(void)
{
    if (!s_lbl_proc) return;
    lv_anim_init(&s_proc_anim);
    lv_anim_set_var(&s_proc_anim, s_lbl_proc);
    lv_anim_set_exec_cb(&s_proc_anim, anim_set_opa);
    lv_anim_set_values(&s_proc_anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&s_proc_anim, 700);
    lv_anim_set_playback_duration(&s_proc_anim, 700);
    lv_anim_set_repeat_count(&s_proc_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_proc_anim);
}

static void stop_proc_anim(void)
{
    lv_anim_delete(s_lbl_proc, anim_set_opa);
    if (s_lbl_proc) lv_obj_set_style_opa(s_lbl_proc, LV_OPA_COVER, 0);
}

/* -------------------------------------------------------------------------
 * Per-frame flush timer — runs inside LVGL task, no lock needed
 * ------------------------------------------------------------------------- */

static void flush_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_chart) return;

    chart_point_t pt;
    bool updated = false;
    while (ring_pop(&pt)) {
        /* Active series = whichever was last reset via scr_scan_reset */
        lv_chart_series_t *ser = s_series[0];  /* single electrode default */
        lv_chart_set_next_value2(s_chart, ser, pt.E_mV, pt.I_uA);

        /* Auto-scale Y axis with 20 % headroom */
        if (pt.I_uA < s_y_min || pt.I_uA > s_y_max) {
            int32_t range = (s_y_max - s_y_min);
            if (range < 1) range = 1;
            int32_t headroom = range / 5;
            if (headroom < 5) headroom = 5;
            if (pt.I_uA < s_y_min) s_y_min = pt.I_uA - headroom;
            if (pt.I_uA > s_y_max) s_y_max = pt.I_uA + headroom;
            lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, s_y_min, s_y_max);
        }

        /* Update live readouts */
        char ibuf[20];
        snprintf(ibuf, sizeof(ibuf), "%+.1f µA", (float)pt.I_uA);
        lv_label_set_text(s_lbl_live_i, ibuf);
        updated = true;
    }

    if (updated) lv_chart_refresh(s_chart);
}

/* -------------------------------------------------------------------------
 * Elapsed time ticker (#13) — runs inside LVGL task, no lock needed
 * ------------------------------------------------------------------------- */

static void elapsed_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_elapsed) return;
    s_elapsed_s++;
    char buf[16];   /* e.g. "99:59" = 5 chars; 16 bytes is safe */
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(s_elapsed_s / 60),
             (unsigned)(s_elapsed_s % 60));
    lv_label_set_text(s_lbl_elapsed, buf);
}

/* -------------------------------------------------------------------------
 * Public — create
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_scan_create(lv_group_t *group)
{
    (void)group;  /* Scan screen: 2-button input handled by abort callback, not focus */

    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* ── Status bar ─────────────────────────────────────────────── */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, LV_HOR_RES, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);  /* #4: 1px bottom divider */
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    s_lbl_elec = lv_label_create(bar);
    lv_label_set_text(s_lbl_elec, "E1  DPV");
    lv_obj_set_style_text_color(s_lbl_elec, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_lbl_elec, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_elec, LV_ALIGN_LEFT_MID, 8, 0);

    s_lbl_progress = lv_label_create(bar);
    lv_label_set_text(s_lbl_progress, "0/---");
    lv_obj_set_style_text_color(s_lbl_progress, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_progress, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_progress, LV_ALIGN_CENTER, 0, 0);

    /* #13: Elapsed time replaces live voltage in bar (voltage shown in readout row) */
    s_lbl_elapsed = lv_label_create(bar);
    lv_label_set_text(s_lbl_elapsed, "0:00");
    lv_obj_set_style_text_color(s_lbl_elapsed, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_elapsed, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_elapsed, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Chart ──────────────────────────────────────────────────── */
    s_chart = lv_chart_create(s_scr);
    lv_obj_set_size(s_chart, LV_HOR_RES - 8, 152);
    lv_obj_set_pos(s_chart, 4, 26);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(s_chart, UI_CHART_MAX_PTS);

    /* Remove default line/point dots for cleaner look at small resolution */
    lv_chart_set_div_line_count(s_chart, 4, 6);

    /* Dark chart background */
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chart, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_pad_all(s_chart, 6, 0);
    lv_obj_set_style_radius(s_chart, 4, 0);
    /* Grid lines */
    lv_obj_set_style_line_color(s_chart, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);

    /* Initial Y range */
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, s_y_min, s_y_max);

    /* Electrode 1 series (teal) — primary */
    s_series[0] = lv_chart_add_series(s_chart,
                                       lv_color_hex(UI_COLOR_ACCENT),
                                       LV_CHART_AXIS_PRIMARY_Y);
    /* Electrode 2 series (amber, hidden until used) */
    s_series[1] = lv_chart_add_series(s_chart,
                                       lv_color_hex(UI_COLOR_PROC),
                                       LV_CHART_AXIS_PRIMARY_Y);
    /* Electrode 3 series (cool indigo — distinct from warm amber on 16-bit panel) #7 */
    s_series[2] = lv_chart_add_series(s_chart,
                                       lv_color_hex(0x7986CBU),
                                       LV_CHART_AXIS_PRIMARY_Y);

    /* #9: Axis labels — inside chart corners, DIM color */
    lv_obj_t *ax_y = lv_label_create(s_scr);
    lv_label_set_text(ax_y, "\u0394I (\u00b5A)");
    lv_obj_set_style_text_color(ax_y, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(ax_y, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ax_y, 8, 30);   /* top-left corner inside chart */

    lv_obj_t *ax_x = lv_label_create(s_scr);
    lv_label_set_text(ax_x, "E (V)");
    lv_obj_set_style_text_color(ax_x, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(ax_x, &lv_font_montserrat_14, 0);
    lv_obj_align(ax_x, LV_ALIGN_TOP_RIGHT, -10, 160);  /* bottom-right of chart */

    /* ── Equilibration spinner (hidden by default) ──────────────── */
    s_spinner = lv_spinner_create(s_scr);
    lv_obj_set_size(s_spinner, 44, 44);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    /* ── Live readout row ───────────────────────────────────────── */
    lv_obj_t *row = lv_obj_create(s_scr);
    lv_obj_set_size(row, LV_HOR_RES, 34);
    lv_obj_set_pos(row, 0, 180);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_radius(row, 0, 0);

    lv_obj_t *i_lbl_hdr = lv_label_create(row);
    lv_label_set_text(i_lbl_hdr, "dI:");
    lv_obj_set_style_text_color(i_lbl_hdr, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(i_lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_align(i_lbl_hdr, LV_ALIGN_LEFT_MID, 4, 0);

    s_lbl_live_i = lv_label_create(row);
    lv_label_set_text(s_lbl_live_i, "+0.0 µA");
    lv_obj_set_style_text_color(s_lbl_live_i, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_lbl_live_i, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_live_i, LV_ALIGN_LEFT_MID, 30, 0);

    /* #13: Live potential stays in readout row (moved from status bar) */
    lv_obj_t *e_lbl_hdr = lv_label_create(row);
    lv_label_set_text(e_lbl_hdr, "E:");
    lv_obj_set_style_text_color(e_lbl_hdr, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(e_lbl_hdr, &lv_font_montserrat_14, 0);
    lv_obj_align(e_lbl_hdr, LV_ALIGN_CENTER, -24, 0);

    lv_obj_t *lbl_live_e_row = lv_label_create(row);
    lv_label_set_text(lbl_live_e_row, "+0.000V");
    lv_obj_set_style_text_color(lbl_live_e_row, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(lbl_live_e_row, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_live_e_row, LV_ALIGN_CENTER, 10, 0);

    s_lbl_proc = lv_label_create(row);
    lv_label_set_text(s_lbl_proc, LV_SYMBOL_REFRESH "  MEASURING");
    lv_obj_set_style_text_color(s_lbl_proc, lv_color_hex(UI_COLOR_PROC), 0);
    lv_obj_set_style_text_font(s_lbl_proc, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_proc, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ── Hint bar ───────────────────────────────────────────────── */
    lv_obj_t *hbar = lv_obj_create(s_scr);
    lv_obj_set_size(hbar, LV_HOR_RES, 26);
    lv_obj_set_pos(hbar, 0, LV_VER_RES - 26);
    lv_obj_set_style_bg_color(hbar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hbar, 0, 0);
    lv_obj_set_style_pad_all(hbar, 0, 0);
    lv_obj_set_style_radius(hbar, 0, 0);

    lv_obj_t *hint = lv_label_create(hbar);
    lv_label_set_text(hint, "Hold to cancel");  /* #8: terse verb form */
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    /* ── Per-frame flush timer ──────────────────────────────────── */
    s_flush_timer = lv_timer_create(flush_timer_cb, 33, NULL);  /* ~30 fps */

    /* Elapsed time ticker — starts paused; scr_scan_reset() enables it */
    s_elapsed_timer = lv_timer_create(elapsed_timer_cb, 1000, NULL);
    lv_timer_pause(s_elapsed_timer);

    ESP_LOGI(TAG, "Scan screen created");
    return s_scr;
}

/* -------------------------------------------------------------------------
 * Public — sink callbacks (called from DispatcherTask, Core 0)
 * All LVGL changes wrapped in lvgl_port_lock by the caller (ui_tft.c sink).
 * These functions only update NON-LVGL state (ring buffer is lock-free).
 * LVGL labels updated directly here since we ARE under lock when called.
 * ------------------------------------------------------------------------- */

void scr_scan_reset(uint8_t electrode)
{
    if (!s_scr) return;

    /* Reset ring buffer */
    s_ring_head = 0;
    s_ring_tail = 0;

    /* Reset Y axis */
    s_y_min = -100;
    s_y_max =  100;

    /* Clear all series data */
    for (int i = 0; i < 3; i++) {
        if (s_series[i]) lv_chart_set_all_value(s_chart, s_series[i], LV_CHART_POINT_NONE);
    }
    if (s_chart) lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, s_y_min, s_y_max);

    /* Update electrode label */
    if (s_lbl_elec) {
        char buf[16];
        if (electrode == 0) {
            snprintf(buf, sizeof(buf), "All  DPV");
        } else {
            snprintf(buf, sizeof(buf), "E%u  DPV", electrode);
        }
        lv_label_set_text(s_lbl_elec, buf);
    }

    /* Reset progress */
    if (s_lbl_progress) lv_label_set_text(s_lbl_progress, "0/---");
    if (s_lbl_live_i)   lv_label_set_text(s_lbl_live_i, "+0.0 µA");
    /* Reset + start elapsed time (#13) */
    s_elapsed_s = 0;
    if (s_lbl_elapsed)  lv_label_set_text(s_lbl_elapsed, "0:00");
    if (s_elapsed_timer) { lv_timer_resume(s_elapsed_timer); lv_timer_reset(s_elapsed_timer); }

    /* Processing state */
    if (s_lbl_proc) {
        lv_label_set_text(s_lbl_proc, LV_SYMBOL_REFRESH "  MEASURING");
        lv_obj_set_style_text_color(s_lbl_proc, lv_color_hex(UI_COLOR_PROC), 0);
        start_proc_anim();
    }
}

void scr_scan_push_point(float E_mV, float I_uA)
{
    ring_push(E_mV, I_uA);
}

void scr_scan_set_progress(uint16_t step, uint16_t total)
{
    if (!s_lbl_progress) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u/%u", step, total);
    lv_label_set_text(s_lbl_progress, buf);
}

void scr_scan_set_equilibrating(bool eq)
{
    if (!s_scr) return;
    if (eq) {
        if (s_spinner)   lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        if (s_chart)     lv_obj_add_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_proc) {
            lv_label_set_text(s_lbl_proc, "Equilibrating...");
            lv_obj_set_style_text_color(s_lbl_proc, lv_color_hex(UI_COLOR_DIM), 0);
            stop_proc_anim();
        }
    } else {
        if (s_spinner) lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        if (s_chart)   lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_proc) {
            lv_label_set_text(s_lbl_proc, LV_SYMBOL_REFRESH "  MEASURING");
            lv_obj_set_style_text_color(s_lbl_proc, lv_color_hex(UI_COLOR_PROC), 0);
            start_proc_anim();
        }
    }
}

void scr_scan_stop_elapsed(void)
{
    if (s_elapsed_timer) lv_timer_pause(s_elapsed_timer);
    stop_proc_anim();
}
