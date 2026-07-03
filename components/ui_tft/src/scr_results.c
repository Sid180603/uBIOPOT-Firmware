/**
 * @file scr_results.c
 * @brief Results screen â€” detected DPV peaks with metal identification and WHO context.
 *
 * Layout (landscape 320x240):
 *   Status bar h=25 (1px bottom divider #4)
 *   Left: mini voltammogram 130x100 (#12)
 *   Right: metal symbol (#ID), name, peak I+E, WHO limit + calibration note
 *   Empty-state (#6): centred "No peaks detected" (20pt DIM) + hint
 *   Metal ID + WHO: echem_core/metal_id.h
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "echem_core/metal_id.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static const char *TAG = "scr_results";

/* -------------------------------------------------------------------------
 * Screen state
 * ------------------------------------------------------------------------- */

static lv_obj_t   *s_scr          = NULL;
static lv_obj_t   *s_lbl_elec     = NULL;
static lv_group_t *s_grp          = NULL;

/* Mini voltammogram chart (#12) */
static lv_obj_t          *s_mini_chart  = NULL;
static lv_chart_series_t *s_mini_series = NULL;

/* Primary peak info labels */
static lv_obj_t *s_lbl_metal_sym  = NULL;
static lv_obj_t *s_lbl_metal_name = NULL;
static lv_obj_t *s_lbl_badge      = NULL;
static lv_obj_t *s_lbl_peak_iv    = NULL;
static lv_obj_t *s_lbl_who        = NULL;
static lv_obj_t *s_lbl_p2         = NULL;

/* Content / empty-state containers */
static lv_obj_t *s_cont_peaks     = NULL;
static lv_obj_t *s_lbl_no_peaks   = NULL;
static lv_obj_t *s_lbl_no_hint    = NULL;

#define MINI_MAX_PTS 300

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void on_run_again(lv_event_t *e) { (void)e; screen_mgr_goto_home(); }
static void on_back_btn(lv_event_t *e)  { (void)e; screen_mgr_goto_home(); }

/* -------------------------------------------------------------------------
 * Public - create
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_results_create(lv_group_t *group)
{
    s_grp = group;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* Status bar with 1px bottom divider (#4) */
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, LV_HOR_RES, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

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

    /* Empty-state: vertically centred in content area (#6) */
    s_lbl_no_peaks = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_no_peaks, "No peaks detected");
    lv_obj_set_style_text_color(s_lbl_no_peaks, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_no_peaks, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_no_peaks, LV_ALIGN_CENTER, 0, -18);
    lv_obj_add_flag(s_lbl_no_peaks, LV_OBJ_FLAG_HIDDEN);

    s_lbl_no_hint = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_no_hint, "Try adjusting concentration or scan range.");
    lv_obj_set_style_text_color(s_lbl_no_hint, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_no_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_no_hint, LV_HOR_RES - 20);
    lv_label_set_long_mode(s_lbl_no_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_no_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_no_hint, LV_ALIGN_CENTER, 0, 12);
    lv_obj_add_flag(s_lbl_no_hint, LV_OBJ_FLAG_HIDDEN);

    /* Peak content container (y=25, h=180) */
    s_cont_peaks = lv_obj_create(s_scr);
    lv_obj_set_size(s_cont_peaks, LV_HOR_RES, 180);
    lv_obj_set_pos(s_cont_peaks, 0, 25);
    lv_obj_set_style_bg_opa(s_cont_peaks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_peaks, 0, 0);
    lv_obj_set_style_pad_all(s_cont_peaks, 0, 0);
    lv_obj_set_style_radius(s_cont_peaks, 0, 0);

    /* Mini voltammogram (#12): 130x100, top-left of content area */
    s_mini_chart = lv_chart_create(s_cont_peaks);
    lv_obj_set_size(s_mini_chart, 130, 100);
    lv_obj_set_pos(s_mini_chart, 4, 8);
    lv_chart_set_type(s_mini_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(s_mini_chart, MINI_MAX_PTS);
    lv_chart_set_div_line_count(s_mini_chart, 2, 3);
    lv_obj_set_style_bg_color(s_mini_chart, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_mini_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mini_chart, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_mini_chart, 1, 0);
    lv_obj_set_style_radius(s_mini_chart, 4, 0);
    lv_obj_set_style_pad_all(s_mini_chart, 4, 0);
    lv_obj_set_style_line_color(s_mini_chart, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    s_mini_series = lv_chart_add_series(s_mini_chart, lv_color_hex(UI_COLOR_ACCENT),
                                         LV_CHART_AXIS_PRIMARY_Y);

    /* Right-side peak info labels (x=140) */
    s_lbl_metal_sym = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_metal_sym, "--");
    lv_obj_set_style_text_color(s_lbl_metal_sym, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_lbl_metal_sym, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(s_lbl_metal_sym, 140, 8);

    s_lbl_metal_name = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_metal_name, "Unknown");
    lv_obj_set_style_text_color(s_lbl_metal_name, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_metal_name, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_metal_name, 140, 34);

    /* Safety badge - right-aligned in container */
    s_lbl_badge = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_badge, "");
    lv_obj_set_style_text_font(s_lbl_badge, &lv_font_montserrat_14, 0);
    lv_obj_align_to(s_lbl_badge, s_cont_peaks, LV_ALIGN_TOP_RIGHT, -8, 8);

    s_lbl_peak_iv = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_peak_iv, "--.-uA  --.---V");
    lv_obj_set_style_text_color(s_lbl_peak_iv, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_lbl_peak_iv, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_peak_iv, 140, 54);

    s_lbl_who = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_who, "");
    lv_obj_set_style_text_color(s_lbl_who, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_who, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_who, 170);
    lv_label_set_long_mode(s_lbl_who, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_who, 140, 74);

    /* Secondary peaks below mini chart */
    s_lbl_p2 = lv_label_create(s_cont_peaks);
    lv_label_set_text(s_lbl_p2, "");
    lv_obj_set_style_text_color(s_lbl_p2, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_p2, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_p2, LV_HOR_RES - 10);
    lv_label_set_long_mode(s_lbl_p2, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_p2, 4, 116);

    /* Hint bar */
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

    /* Back button (transparent, for group focus) */
    lv_obj_t *btn_back = lv_btn_create(hbar);
    lv_obj_set_size(btn_back, 80, 26);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, on_back_btn, LV_EVENT_CLICKED, NULL);
    if (group) lv_group_add_obj(group, btn_back);

    /* Run Again button */
    lv_obj_t *btn_run = lv_btn_create(hbar);
    lv_obj_set_size(btn_run, 120, 28);
    lv_obj_align(btn_run, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_run, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(btn_run, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_run, 6, 0);
    lv_obj_add_event_cb(btn_run, on_run_again, LV_EVENT_CLICKED, NULL);
    if (group) lv_group_add_obj(group, btn_run);
    lv_obj_t *run_lbl = lv_label_create(btn_run);
    lv_label_set_text(run_lbl, LV_SYMBOL_REFRESH "  Run Again");
    lv_obj_set_style_text_color(run_lbl, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_font(run_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(run_lbl);

    ESP_LOGI(TAG, "Results screen created");
    return s_scr;
}

/* -------------------------------------------------------------------------
 * Public - update
 * ------------------------------------------------------------------------- */

void scr_results_set(const peak_t *peaks, uint16_t n_peaks, uint8_t electrode)
{
    if (!s_scr) return;

    char ebuf[16];
    if (electrode == 0) {
        snprintf(ebuf, sizeof(ebuf), "All  DPV");
    } else {
        snprintf(ebuf, sizeof(ebuf), "E%u  DPV", electrode);
    }
    lv_label_set_text(s_lbl_elec, ebuf);

    if (n_peaks == 0) {
        /* #6: empty state - centred */
        lv_obj_add_flag(s_cont_peaks, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_no_peaks, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_no_hint,  LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_cont_peaks, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_no_peaks, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_no_hint,  LV_OBJ_FLAG_HIDDEN);

    /* Primary peak */
    const metal_profile_t *metal = metal_identify(peaks[0].E_mV);

    if (metal) {
        lv_label_set_text(s_lbl_metal_sym,  metal->symbol);
        lv_label_set_text(s_lbl_metal_name, metal->name);
        char who_buf[64];
        if (metal->who_limit_ugL > 0.0f) {
            snprintf(who_buf, sizeof(who_buf), "WHO: %.0f ug/L  [Not calibrated]",
                     metal->who_limit_ugL);
        } else {
            snprintf(who_buf, sizeof(who_buf), "No WHO numeric guideline");
        }
        lv_label_set_text(s_lbl_who, who_buf);
        lv_label_set_text(s_lbl_badge, "");   /* filled by P8 calibration */
    } else {
        lv_label_set_text(s_lbl_metal_sym,  "??");
        lv_label_set_text(s_lbl_metal_name, "Unknown metal");
        lv_label_set_text(s_lbl_who, "Peak outside reference windows");
        lv_label_set_text(s_lbl_badge, "");
    }

    char ivbuf[40];
    snprintf(ivbuf, sizeof(ivbuf), "%+.1f uA   %.3f V",
             peaks[0].I_uA, peaks[0].E_mV / 1000.0f);
    lv_label_set_text(s_lbl_peak_iv, ivbuf);

    /* Secondary peaks */
    if (n_peaks > 1) {
        char more[160] = "";
        int  pos = 0;
        for (uint16_t i = 1; i < n_peaks && i < 4; i++) {
            const metal_profile_t *m2 = metal_identify(peaks[i].E_mV);
            pos += snprintf(more + pos, sizeof(more) - (size_t)pos,
                            "P%u: %+.1f uA @ %.3f V  %s\n",
                            i + 1, peaks[i].I_uA, peaks[i].E_mV / 1000.0f,
                            m2 ? m2->symbol : "??");
        }
        lv_label_set_text(s_lbl_p2, more);
    } else {
        lv_label_set_text(s_lbl_p2, "");
    }
}

void scr_results_set_curve(const float *E_mV, const float *I_uA, uint16_t n)
{
    if (!s_mini_chart || !s_mini_series || n == 0) return;

    lv_chart_set_all_value(s_mini_chart, s_mini_series, LV_CHART_POINT_NONE);

    uint16_t step = (n > MINI_MAX_PTS) ? (n / MINI_MAX_PTS) : 1;

    float e_min = E_mV[0], e_max = E_mV[0];
    float i_min = I_uA[0], i_max = I_uA[0];
    for (uint16_t i = 0; i < n; i++) {
        if (E_mV[i] < e_min) e_min = E_mV[i];
        if (E_mV[i] > e_max) e_max = E_mV[i];
        if (I_uA[i] < i_min) i_min = I_uA[i];
        if (I_uA[i] > i_max) i_max = I_uA[i];
    }
    float e_rng = (e_max - e_min) * 0.1f;
    float i_rng = (i_max - i_min) * 0.1f;
    /* BUG 6 fix: use sensible minimum ranges for DPV context.
     * 1.0f guard gave a 2 mV / 2 µA axis on flat-line scans — unreadable.
     * 50 mV / 5 µA gives at least a 100 mV / 10 µA visible window. */
    if (e_rng < 50.0f) e_rng = 50.0f;
    if (i_rng <  5.0f) i_rng =  5.0f;
    lv_chart_set_axis_range(s_mini_chart, LV_CHART_AXIS_PRIMARY_X,
                            (int32_t)(e_min - e_rng), (int32_t)(e_max + e_rng));
    lv_chart_set_axis_range(s_mini_chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)(i_min - i_rng), (int32_t)(i_max + i_rng));

    for (uint16_t i = 0; i < n; i += step) {
        lv_chart_set_next_value2(s_mini_chart, s_mini_series,
                                 (int32_t)E_mV[i], (int32_t)I_uA[i]);
    }
    lv_chart_refresh(s_mini_chart);
}

