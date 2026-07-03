/**
 * @file scr_settings.c
 * @brief Settings screen — WiFi info, electrode select, zero trigger.
 *
 * WiFi information is stubbed in P4 ("Not configured — P5").
 * net_comms (P5) calls ui_tft_set_wifi_info() to fill in real data.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "scr_settings";

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_lbl_ssid   = NULL;
static lv_obj_t *s_lbl_ip     = NULL;
static lv_obj_t *s_lbl_url    = NULL;
static lv_group_t *s_grp      = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static lv_obj_t *make_row(lv_obj_t *parent, int y_pos,
                           const char *key, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_HOR_RES - 16, 22);
    lv_obj_set_pos(row, 8, y_pos);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "---");
    lv_obj_set_style_text_color(v, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, 200);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    if (val_out) *val_out = v;

    return row;
}

static void on_zero_btn(lv_event_t *e)
{
    (void)e;
    /* P8: trigger auto-zero via engine_zero() */
    /* For now, just show a toast acknowledging the tap */
    screen_mgr_show_toast("Zero: coming in P8 (calibration)", TOAST_INFO);
}

static void on_back_btn(lv_event_t *e)
{
    (void)e;
    screen_mgr_goto_home();
}

/* -------------------------------------------------------------------------
 * Public — create
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_settings_create(lv_group_t *group)
{
    s_grp = group;
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

    lv_obj_t *bar_title = lv_label_create(bar);
    lv_label_set_text(bar_title, "SETTINGS");
    lv_obj_set_style_text_color(bar_title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(bar_title, &lv_font_montserrat_14, 0);
    lv_obj_align(bar_title, LV_ALIGN_LEFT_MID, 8, 0);

    /* ── WiFi section header ────────────────────────────────────── */
    lv_obj_t *wifi_hdr = lv_label_create(s_scr);
    lv_label_set_text(wifi_hdr, LV_SYMBOL_WIFI "  WiFi");
    lv_obj_set_style_text_color(wifi_hdr, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(wifi_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(wifi_hdr, 8, 30);

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_scr);
    lv_obj_set_size(div, LV_HOR_RES - 16, 1);
    lv_obj_set_pos(div, 8, 48);
    lv_obj_set_style_bg_color(div, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);

    make_row(s_scr, 52,  "SSID:", &s_lbl_ssid);
    make_row(s_scr, 76,  "IP:  ", &s_lbl_ip);
    make_row(s_scr, 100, "URL: ", &s_lbl_url);

    /* QR code placeholder (P5 wires real SSID/IP here) */
    lv_obj_t *qr_note = lv_label_create(s_scr);
    lv_label_set_text(qr_note, "[ QR — available after WiFi init (P5) ]");
    lv_obj_set_style_text_color(qr_note, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(qr_note, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(qr_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(qr_note, LV_HOR_RES - 16);
    lv_obj_set_pos(qr_note, 8, 124);

    /* ── Calibration section header (#16) ───────────────────────── */
    lv_obj_t *cal_hdr = lv_label_create(s_scr);
    lv_label_set_text(cal_hdr, LV_SYMBOL_EDIT "  Calibration");
    lv_obj_set_style_text_color(cal_hdr, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(cal_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cal_hdr, 8, 152);

    lv_obj_t *cal_div = lv_obj_create(s_scr);
    lv_obj_set_size(cal_div, LV_HOR_RES - 16, 1);
    lv_obj_set_pos(cal_div, 8, 169);
    lv_obj_set_style_bg_color(cal_div, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(cal_div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cal_div, 0, 0);

    /* Zero button + calibration note */
    lv_obj_t *btn_zero = lv_btn_create(s_scr);
    lv_obj_set_size(btn_zero, 100, 26);
    lv_obj_set_pos(btn_zero, 8, 173);
    lv_obj_set_style_bg_color(btn_zero, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(btn_zero, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn_zero, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(btn_zero, 1, 0);
    lv_obj_set_style_radius(btn_zero, 6, 0);
    lv_obj_add_event_cb(btn_zero, on_zero_btn, LV_EVENT_CLICKED, NULL);
    if (group) lv_group_add_obj(group, btn_zero);
    lv_obj_t *zero_lbl = lv_label_create(btn_zero);
    lv_label_set_text(zero_lbl, LV_SYMBOL_POWER "  Zero");
    lv_obj_set_style_text_color(zero_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(zero_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(zero_lbl);

    lv_obj_t *cal_note = lv_label_create(s_scr);
    lv_label_set_text(cal_note, "[ Not calibrated ]");
    lv_obj_set_style_text_color(cal_note, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(cal_note, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cal_note, 116, 178);

    /* ── Hint bar ───────────────────────────────────────────────── */
    lv_obj_t *hbar = lv_obj_create(s_scr);
    lv_obj_set_size(hbar, LV_HOR_RES, 30);
    lv_obj_set_pos(hbar, 0, LV_VER_RES - 30);
    lv_obj_set_style_bg_color(hbar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hbar, 0, 0);
    lv_obj_set_style_pad_all(hbar, 2, 0);
    lv_obj_set_style_radius(hbar, 0, 0);

    lv_obj_t *btn_back = lv_btn_create(hbar);
    lv_obj_set_size(btn_back, 80, 26);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn_back, 4, 0);
    lv_obj_add_event_cb(btn_back, on_back_btn, LV_EVENT_CLICKED, NULL);
    if (group) lv_group_add_obj(group, btn_back);

    lv_obj_t *back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);

    /* Default values (stub) */
    lv_label_set_text(s_lbl_ssid, "Not configured");
    lv_label_set_text(s_lbl_ip,   "---");
    lv_label_set_text(s_lbl_url,  "---");

    ESP_LOGI(TAG, "Settings screen created");
    return s_scr;
}

/* -------------------------------------------------------------------------
 * Public — update WiFi info (called from P5 / ui_tft_set_wifi_info)
 * ------------------------------------------------------------------------- */

void scr_settings_set_wifi(const char *ssid, const char *ip, const char *url)
{
    if (!s_scr) return;
    if (ssid) lv_label_set_text(s_lbl_ssid, ssid);
    if (ip)   lv_label_set_text(s_lbl_ip,   ip);
    if (url)  lv_label_set_text(s_lbl_url,  url);
}
