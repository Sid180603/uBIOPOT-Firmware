/**
 * @file scr_home.c
 * @brief Home / Menu screen — device status, electrode select, menu items.
 *
 * Layout (landscape 320×240):
 *   ┌──────────────────────────────────────────────────┐
 *   │  STATUS BAR  Aqua-HMET      ● READY    E1  WiFi  │  h=24
 *   ├──────────────────────────────────────────────────┤
 *   │  > Start DPV                                      │  ← focused item (teal)
 *   │    Electrode: 1                                   │
 *   │    View Params                                    │
 *   │    Settings                                       │
 *   │    About                                          │
 *   ├──────────────────────────────────────────────────┤
 *   │  [NAV: cycle]          [START: select/run]        │  h=30
 *   └──────────────────────────────────────────────────┘
 *
 * Electrode selection cycles through 1 / 2 / 3 / All when that menu item is activated.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "acq_engine.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "scr_home";

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static lv_obj_t  *s_scr         = NULL;
static lv_obj_t  *s_lbl_status  = NULL;   /* "● READY" / "● RUNNING" etc */
static lv_obj_t  *s_lbl_elec    = NULL;   /* "Electrode: 1" in status bar */
static lv_group_t *s_grp        = NULL;
static uint8_t    s_electrode   = 1;      /* Selected electrode (1-3; 0=All) */

/* Menu item indices */
#define MENU_START_DPV   0
#define MENU_ELECTRODE   1
#define MENU_PARAMS      2
#define MENU_SETTINGS    3
#define MENU_ABOUT       4
#define MENU_COUNT       5

static lv_obj_t  *s_menu_items[MENU_COUNT];

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static void menu_item_click(lv_event_t *e);
static void update_electrode_label(void);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static lv_obj_t *create_menu_item(lv_obj_t *parent, const char *text,
                                   int idx, int y_pos)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_HOR_RES - 16, 30);
    lv_obj_set_pos(btn, 8, y_pos);

    /* Normal state: transparent bg, dim border on bottom */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_add_event_cb(btn, menu_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (s_grp) lv_group_add_obj(s_grp, btn);

    return btn;
}

static void update_status_label(scan_state_t state)
{
    if (!s_lbl_status) return;
    switch (state) {
        case SCAN_STATE_IDLE:
        case SCAN_STATE_COMPLETE:
            lv_label_set_text(s_lbl_status, LV_SYMBOL_OK "  READY");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_READY), 0);
            break;
        case SCAN_STATE_EQUILIBRATING:
        case SCAN_STATE_RUNNING:
            lv_label_set_text(s_lbl_status, LV_SYMBOL_REFRESH "  RUNNING");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_PROC), 0);
            break;
        case SCAN_STATE_ABORTING:
            lv_label_set_text(s_lbl_status, LV_SYMBOL_CLOSE "  ABORTING");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_ABORT), 0);
            break;
        case SCAN_STATE_ERROR:
            lv_label_set_text(s_lbl_status, LV_SYMBOL_WARNING "  ERROR");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_ABORT), 0);
            break;
    }
}

static void update_electrode_label(void)
{
    if (!s_lbl_elec || !s_menu_items[MENU_ELECTRODE]) return;
    char ebuf[24];
    if (s_electrode == 0) {
        snprintf(ebuf, sizeof(ebuf), "Electrode:  All");
    } else {
        snprintf(ebuf, sizeof(ebuf), "Electrode:  %u", s_electrode);
    }
    /* Update status bar electrode label */
    char sbuf[8];
    snprintf(sbuf, sizeof(sbuf), s_electrode == 0 ? "All" : "E%u", s_electrode);
    lv_label_set_text(s_lbl_elec, sbuf);
    /* Update the menu item label child */
    lv_obj_t *lbl = lv_obj_get_child(s_menu_items[MENU_ELECTRODE], 0);
    if (lbl) lv_label_set_text(lbl, ebuf);
}

/* -------------------------------------------------------------------------
 * Event handlers
 * ------------------------------------------------------------------------- */

static void menu_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch (idx) {
        case MENU_START_DPV: {
            /* Build default DPV params and start */
            dpv_params_t p = {
                .e_begin_mV         = -600.0f,
                .e_end_mV           =  600.0f,
                .e_step_mV          =    5.0f,
                .e_pulse_mV         =   25.0f,
                .t_pulse_ms         =   50,
                .t_period_ms        =  200,
                .t_equilibration_ms = 2000,
                .cycles             =    1,
                .n_avg              =    5,
                .electrode          = (electrode_t)s_electrode,
            };
            esp_err_t ret = engine_start(s_electrode, &p);
            if (ret == ESP_OK) {
                screen_mgr_goto_scan();
            } else {
                screen_mgr_show_toast("Start failed — check electrode");
            }
            break;
        }
        case MENU_ELECTRODE:
            /* Cycle: 1 → 2 → 3 → 0(All) → 1 */
            if (s_electrode == 0 || s_electrode > 3) s_electrode = 1;
            else if (s_electrode == 3) {
                s_electrode = 0;  /* after 3, select All */
            } else {
                s_electrode++;
            }
            update_electrode_label();
            break;
        case MENU_PARAMS:
            screen_mgr_show_toast("Params: set via WiFi web app or serial");
            break;
        case MENU_SETTINGS:
            screen_mgr_goto_settings();
            break;
        case MENU_ABOUT:
            screen_mgr_show_toast("Aqua-HMET v1  |  BITS Pilani 2026");
            break;
    }
}

/* -------------------------------------------------------------------------
 * Public — create
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_home_create(lv_group_t *group)
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
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    lv_obj_t *bar_name = lv_label_create(bar);
    lv_label_set_text(bar_name, "Aqua-HMET");
    lv_obj_set_style_text_color(bar_name, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(bar_name, &lv_font_montserrat_14, 0);
    lv_obj_align(bar_name, LV_ALIGN_LEFT_MID, 8, 0);

    s_lbl_status = lv_label_create(bar);
    lv_label_set_text(s_lbl_status, LV_SYMBOL_OK "  READY");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_READY), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 0);

    s_lbl_elec = lv_label_create(bar);
    lv_label_set_text(s_lbl_elec, "E1");
    lv_obj_set_style_text_color(s_lbl_elec, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_elec, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_elec, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Menu items ─────────────────────────────────────────────── */
    const char *labels[MENU_COUNT] = {
        LV_SYMBOL_PLAY "  Start DPV",
        "Electrode:  1",
        LV_SYMBOL_LIST "  View Params",
        LV_SYMBOL_SETTINGS "  Settings",
        LV_SYMBOL_HOME "  About",
    };
    for (int i = 0; i < MENU_COUNT; i++) {
        s_menu_items[i] = create_menu_item(s_scr, labels[i], i, 28 + i * 34);
    }

    /* ── Hint bar ───────────────────────────────────────────────── */
    lv_obj_t *hbar = lv_obj_create(s_scr);
    lv_obj_set_size(hbar, LV_HOR_RES, 30);
    lv_obj_set_pos(hbar, 0, LV_VER_RES - 30);
    lv_obj_set_style_bg_color(hbar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hbar, 0, 0);
    lv_obj_set_style_pad_all(hbar, 0, 0);
    lv_obj_set_style_radius(hbar, 0, 0);

    lv_obj_t *hint_l = lv_label_create(hbar);
    lv_label_set_text(hint_l, LV_SYMBOL_UP LV_SYMBOL_DOWN "  NAV: cycle");
    lv_obj_set_style_text_color(hint_l, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(hint_l, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_l, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *hint_r = lv_label_create(hbar);
    lv_label_set_text(hint_r, "START: select  " LV_SYMBOL_OK);
    lv_obj_set_style_text_color(hint_r, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(hint_r, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_r, LV_ALIGN_RIGHT_MID, -8, 0);

    ESP_LOGI(TAG, "Home screen created");
    return s_scr;
}

/* -------------------------------------------------------------------------
 * Public — update
 * ------------------------------------------------------------------------- */

void scr_home_set_state(scan_state_t state)
{
    if (!s_scr) return;
    update_status_label(state);
}

void scr_home_set_electrode(uint8_t electrode)
{
    s_electrode = electrode;
    if (s_scr) update_electrode_label();
}

uint8_t scr_home_get_electrode(void)
{
    return s_electrode;
}
