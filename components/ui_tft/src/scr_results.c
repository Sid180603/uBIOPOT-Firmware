/**
 * @file scr_results.c
 * @brief Results screen — shows detected DPV peaks after scan complete.
 *
 * Layout (landscape 320×240):
 *   ┌──────────────────────────────────────────────────┐
 *   │  STATUS BAR                                       │  h=24
 *   ├──────────────────────────────────────────────────┤
 *   │  ▲ PEAK CURRENT              ▲ PEAK POTENTIAL    │  h=80
 *   │    +42.3 µA                    -0.421 V          │
 *   ├──────────────────────────────────────────────────┤
 *   │  (secondary peaks if any, in smaller font)       │  h=80
 *   ├──────────────────────────────────────────────────┤
 *   │  [< Back]             [Run Again]                │  h=30 (hint bar)
 *   └──────────────────────────────────────────────────┘
 *
 * If concentration slope is known (from NVS, P8), "≈X µM" shown next to peak I.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

static const char *TAG = "scr_results";

/* -------------------------------------------------------------------------
 * Screen state
 * ------------------------------------------------------------------------- */

static lv_obj_t *s_scr       = NULL;
static lv_obj_t *s_lbl_elec  = NULL;  /* status bar: "Electrode 1  DPV" */
static lv_obj_t *s_cont_main = NULL;  /* main content area */
static lv_obj_t *s_lbl_peak1_i   = NULL;
static lv_obj_t *s_lbl_peak1_e   = NULL;
static lv_obj_t *s_lbl_peak1_hdr = NULL;
static lv_obj_t *s_lbl_more  = NULL;  /* secondary peaks summary */
static lv_group_t *s_grp     = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void style_status_bar(lv_obj_t *bar)
{
    lv_obj_set_size(bar, LV_HOR_RES, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_align_t align, int x_ofs)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 120, 28);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

static void on_run_again(lv_event_t *e)
{
    (void)e;
    /* Navigate to home where user can re-press Start */
    screen_mgr_goto_home();
}

/* -------------------------------------------------------------------------
 * Public — create (called once from screen_mgr_init)
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_results_create(lv_group_t *group)
{
    s_grp = group;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* ── Status bar ─────────────────────────────────────────────── */
    lv_obj_t *bar = lv_obj_create(s_scr);
    style_status_bar(bar);

    lv_obj_t *bar_title = lv_label_create(bar);
    lv_label_set_text(bar_title, "RESULTS");
    lv_obj_set_style_text_color(bar_title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(bar_title, &lv_font_montserrat_14, 0);
    lv_obj_align(bar_title, LV_ALIGN_LEFT_MID, 8, 0);

    s_lbl_elec = lv_label_create(bar);
    lv_label_set_text(s_lbl_elec, "E1  DPV");
    lv_obj_set_style_text_color(s_lbl_elec, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_elec, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_elec, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Main peak block ────────────────────────────────────────── */
    s_cont_main = lv_obj_create(s_scr);
    lv_obj_set_size(s_cont_main, LV_HOR_RES, 106);
    lv_obj_set_pos(s_cont_main, 0, 24);
    lv_obj_set_style_bg_color(s_cont_main, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_cont_main, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cont_main, 0, 0);
    lv_obj_set_style_pad_all(s_cont_main, 0, 0);
    lv_obj_set_style_radius(s_cont_main, 0, 0);

    s_lbl_peak1_hdr = lv_label_create(s_cont_main);
    lv_label_set_text(s_lbl_peak1_hdr, "PEAK 1");
    lv_obj_set_style_text_color(s_lbl_peak1_hdr, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_peak1_hdr, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_peak1_hdr, LV_ALIGN_TOP_LEFT, 10, 8);

    /* Peak current — big */
    s_lbl_peak1_i = lv_label_create(s_cont_main);
    lv_label_set_text(s_lbl_peak1_i, "--.-  µA");
    lv_obj_set_style_text_color(s_lbl_peak1_i, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_lbl_peak1_i, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_peak1_i, LV_ALIGN_TOP_LEFT, 10, 26);

    /* Peak potential — big */
    s_lbl_peak1_e = lv_label_create(s_cont_main);
    lv_label_set_text(s_lbl_peak1_e, "--.-  V");
    lv_obj_set_style_text_color(s_lbl_peak1_e, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_lbl_peak1_e, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_peak1_e, LV_ALIGN_TOP_RIGHT, -10, 26);

    /* ── Secondary peaks summary ──────────────────────────────── */
    s_lbl_more = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_more, "");
    lv_obj_set_style_text_color(s_lbl_more, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_more, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_more, LV_HOR_RES - 20);
    lv_label_set_long_mode(s_lbl_more, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_more, 10, 134);

    /* ── Hint bar ───────────────────────────────────────────────── */
    lv_obj_t *hbar = lv_obj_create(s_scr);
    lv_obj_set_size(hbar, LV_HOR_RES, 30);
    lv_obj_set_pos(hbar, 0, LV_VER_RES - 30);
    lv_obj_set_style_bg_color(hbar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hbar, 0, 0);
    lv_obj_set_style_pad_all(hbar, 2, 0);
    lv_obj_set_style_radius(hbar, 0, 0);

    lv_obj_t *hint_l = lv_label_create(hbar);
    lv_label_set_text(hint_l, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(hint_l, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(hint_l, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_l, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *btn_run = make_btn(hbar, LV_SYMBOL_REFRESH "  Run Again",
                                 LV_ALIGN_RIGHT_MID, -4);
    lv_obj_add_event_cb(btn_run, on_run_again, LV_EVENT_CLICKED, NULL);
    if (group) lv_group_add_obj(group, btn_run);

    ESP_LOGI(TAG, "Results screen created");
    return s_scr;
}

/* -------------------------------------------------------------------------
 * Public — update (called from screen_mgr under lock)
 * ------------------------------------------------------------------------- */

void scr_results_set(const peak_t *peaks, uint16_t n_peaks, uint8_t electrode)
{
    if (!s_scr) return;

    /* Update electrode label in status bar */
    char ebuf[16];
    if (electrode == 0) {
        snprintf(ebuf, sizeof(ebuf), "All  DPV");
    } else {
        snprintf(ebuf, sizeof(ebuf), "E%u  DPV", electrode);
    }
    lv_label_set_text(s_lbl_elec, ebuf);

    if (n_peaks == 0) {
        lv_label_set_text(s_lbl_peak1_hdr, "NO PEAKS DETECTED");
        lv_label_set_text(s_lbl_peak1_i, "--");
        lv_label_set_text(s_lbl_peak1_e, "--");
        lv_label_set_text(s_lbl_more, "");
        return;
    }

    /* Primary peak */
    char ibuf[32], ebuf2[32];
    snprintf(ibuf, sizeof(ibuf), "%+.1f µA", peaks[0].I_uA);
    snprintf(ebuf2, sizeof(ebuf2), "%.3f V", peaks[0].E_mV / 1000.0f);
    lv_label_set_text(s_lbl_peak1_hdr, "PEAK 1");
    lv_label_set_text(s_lbl_peak1_i, ibuf);
    lv_label_set_text(s_lbl_peak1_e, ebuf2);

    /* Secondary peaks (up to 3 more, compact) */
    if (n_peaks > 1) {
        char more_buf[128] = "";
        int pos = 0;
        for (uint16_t i = 1; i < n_peaks && i < 4; i++) {
            pos += snprintf(more_buf + pos, sizeof(more_buf) - (size_t)pos,
                            "P%u: %+.1f µA @ %.3f V\n",
                            i + 1, peaks[i].I_uA, peaks[i].E_mV / 1000.0f);
        }
        lv_label_set_text(s_lbl_more, more_buf);
    } else {
        lv_label_set_text(s_lbl_more, "");
    }
}
