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
#include "echem_core/dpv.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "scr_home";

/* #15 animation wrapper — set dot opacity */
static void dot_set_opa(void *obj, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static lv_obj_t  *s_scr         = NULL;
static lv_obj_t  *s_dot_status  = NULL;   /* 10×10 circle: green/amber/red         */
static lv_obj_t  *s_lbl_status  = NULL;   /* "READY" / "RUNNING" text              */
static lv_obj_t  *s_lbl_elec    = NULL;   /* "Electrode: 1" in status bar          */
static lv_group_t *s_grp        = NULL;
static uint8_t    s_electrode   = 1;      /* Selected electrode (1-3; 0=All) */
static float      s_e_begin_mV = -900.0f; /* Last-used DPV E range (mV) */
static float      s_e_end_mV   =  500.0f;

/* #15 — pulse animation handle (kept for cancel on state change) */
static lv_anim_t  s_dot_anim;

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

/**
 * Create a standard menu button inside a flex container.
 * @param parent   Flex container (not the screen directly).
 * @param text     Label text.
 * @param idx      Menu index passed back via user_data.
 * @param primary  If true, use 20pt font (#1) and 2px accent left-border (#2).
 */
static lv_obj_t *create_menu_item(lv_obj_t *parent, const char *text,
                                   int idx, bool primary)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_HOR_RES - 16, 34);

    /* Normal state */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);

    /* #2: accent 2px left border for primary action (Start DPV) */
    if (primary) {
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
    }

    /* Focused state */
    lv_obj_set_style_border_width(btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    /* #1: 20pt for primary action (Start DPV) */
    lv_obj_set_style_text_font(lbl, primary ? &lv_font_montserrat_20 : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_add_event_cb(btn, menu_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    if (s_grp) lv_group_add_obj(s_grp, btn);

    return btn;
}

static void update_status_label(scan_state_t state)
{
    if (!s_dot_status || !s_lbl_status) return;

    /* Stop any existing dot pulse animation */
    lv_anim_delete(s_dot_status, dot_set_opa);
    lv_obj_set_style_opa(s_dot_status, LV_OPA_COVER, 0);

    switch (state) {
        case SCAN_STATE_IDLE:
        case SCAN_STATE_COMPLETE:
            lv_obj_set_style_bg_color(s_dot_status, lv_color_hex(UI_COLOR_READY), 0);
            lv_label_set_text(s_lbl_status, "READY");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_READY), 0);
            break;
        case SCAN_STATE_EQUILIBRATING:
        case SCAN_STATE_RUNNING: {
            lv_obj_set_style_bg_color(s_dot_status, lv_color_hex(UI_COLOR_PROC), 0);
            lv_label_set_text(s_lbl_status, "RUNNING");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_PROC), 0);
            /* Pulse the dot: opacity 255→60→255, 1500 ms cycle */
            lv_anim_init(&s_dot_anim);
            lv_anim_set_var(&s_dot_anim, s_dot_status);
            lv_anim_set_exec_cb(&s_dot_anim, dot_set_opa);
            lv_anim_set_values(&s_dot_anim, LV_OPA_COVER, LV_OPA_20);
            lv_anim_set_duration(&s_dot_anim, 750);
            lv_anim_set_playback_duration(&s_dot_anim, 750);
            lv_anim_set_repeat_count(&s_dot_anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&s_dot_anim);
            break;
        }
        case SCAN_STATE_ABORTING:
            lv_obj_set_style_bg_color(s_dot_status, lv_color_hex(UI_COLOR_ABORT), 0);
            lv_label_set_text(s_lbl_status, "ABORTING");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_ABORT), 0);
            /* Fast blink for abort */
            lv_anim_init(&s_dot_anim);
            lv_anim_set_var(&s_dot_anim, s_dot_status);
            lv_anim_set_exec_cb(&s_dot_anim, dot_set_opa);
            lv_anim_set_values(&s_dot_anim, LV_OPA_COVER, LV_OPA_TRANSP);
            lv_anim_set_duration(&s_dot_anim, 300);
            lv_anim_set_playback_duration(&s_dot_anim, 300);
            lv_anim_set_repeat_count(&s_dot_anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&s_dot_anim);
            break;
        case SCAN_STATE_ERROR:
            lv_obj_set_style_bg_color(s_dot_status, lv_color_hex(UI_COLOR_ABORT), 0);
            lv_label_set_text(s_lbl_status, "ERROR");
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_ABORT), 0);
            break;
    }
}

static void update_electrode_label(void)
{
    if (!s_lbl_elec || !s_menu_items[MENU_ELECTRODE]) return;

    /* Update status bar electrode label */
    char sbuf[8];
    snprintf(sbuf, sizeof(sbuf), s_electrode == 0 ? "All" : "E%u", s_electrode);
    lv_label_set_text(s_lbl_elec, sbuf);

    /* #3: Electrode item has TWO children — key label (child 0) + value label (child 1).
     * Update the right-aligned value label only. */
    lv_obj_t *val_lbl = lv_obj_get_child(s_menu_items[MENU_ELECTRODE], 1);
    if (val_lbl) {
        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), s_electrode == 0 ? "All" : "%u", s_electrode);
        lv_label_set_text(val_lbl, vbuf);
    }
}

/* -------------------------------------------------------------------------
 * Event handlers
 * ------------------------------------------------------------------------- */

static void menu_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch (idx) {
        case MENU_START_DPV: {
            /* Use the shared canonical defaults; only the electrode varies.
             * Single source of truth = DPV_PARAMS_DEFAULT (matches web + serial). */
            dpv_params_t p = DPV_PARAMS_DEFAULT;
            p.electrode = (electrode_t)s_electrode;
            s_e_begin_mV = p.e_begin_mV;
            s_e_end_mV   = p.e_end_mV;
            esp_err_t ret = engine_start(s_electrode, &p);
            if (ret == ESP_OK) {
                screen_mgr_goto_scan();
            } else {
                screen_mgr_show_toast("Start failed \u2014 check electrode", TOAST_ERROR);
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
            screen_mgr_show_toast("Params: set via WiFi web app or serial", TOAST_INFO);
            break;
        case MENU_SETTINGS:
            screen_mgr_goto_settings();
            break;
        case MENU_ABOUT:
            screen_mgr_show_toast("Aqua-HMET v1  |  BITS Pilani 2026", TOAST_INFO);
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
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);  /* #4: 1px bottom divider */
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    lv_obj_t *bar_name = lv_label_create(bar);
    lv_label_set_text(bar_name, "Aqua-HMET");
    lv_obj_set_style_text_color(bar_name, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(bar_name, &lv_font_montserrat_14, 0);
    lv_obj_align(bar_name, LV_ALIGN_LEFT_MID, 8, 0);

    /* #15: Status dot (10×10 circle) + text label.
     * Use LEFT_MID absolute offsets so the dot never overlaps the text.
     * Dot spans x=108..118, label starts at x=124 (6px gap). */
    s_dot_status = lv_obj_create(bar);
    lv_obj_set_size(s_dot_status, 10, 10);
    lv_obj_set_style_radius(s_dot_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot_status, lv_color_hex(UI_COLOR_READY), 0);
    lv_obj_set_style_bg_opa(s_dot_status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot_status, 0, 0);
    lv_obj_align(s_dot_status, LV_ALIGN_LEFT_MID, 108, 0);

    s_lbl_status = lv_label_create(bar);
    lv_label_set_text(s_lbl_status, "READY");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_READY), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_LEFT_MID, 124, 0);

    s_lbl_elec = lv_label_create(bar);
    lv_label_set_text(s_lbl_elec, "E1");
    lv_obj_set_style_text_color(s_lbl_elec, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_elec, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_elec, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── Menu flex container (#10) ───────────────────────────────── */
    /* Sits between status bar (h=24) and hint bar (h=30, at y=210).
     * Flex column distributes 5 items with even row gap.                */
    lv_obj_t *menu_cont = lv_obj_create(s_scr);
    lv_obj_set_size(menu_cont, LV_HOR_RES, LV_VER_RES - 24 - 30);
    lv_obj_set_pos(menu_cont, 0, 25);  /* 1px below bar divider */
    lv_obj_set_style_bg_opa(menu_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_cont, 0, 0);
    lv_obj_set_style_pad_all(menu_cont, 0, 0);
    lv_obj_set_style_pad_row(menu_cont, 2, 0);
    lv_obj_set_style_radius(menu_cont, 0, 0);
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Start DPV — primary action: 20pt font + accent left border */
    s_menu_items[MENU_START_DPV] = create_menu_item(menu_cont,
                                    LV_SYMBOL_PLAY "  Start DPV", MENU_START_DPV, true);

    /* Electrode — key-value toggle (#3): "Electrode" left, "1" right in accent */
    {
        lv_obj_t *btn = lv_btn_create(menu_cont);
        lv_obj_set_size(btn, LV_HOR_RES - 16, 34);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_FOCUS), LV_STATE_FOCUSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        /* Key label — left */
        lv_obj_t *k = lv_label_create(btn);
        lv_label_set_text(k, LV_SYMBOL_LOOP "  Electrode");
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_color(k, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 8, 0);
        /* Value label — right in accent, signals "this is changeable" */
        lv_obj_t *v = lv_label_create(btn);
        lv_label_set_text(v, "1");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_event_cb(btn, menu_item_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)MENU_ELECTRODE);
        if (s_grp) lv_group_add_obj(s_grp, btn);
        s_menu_items[MENU_ELECTRODE] = btn;
    }

    /* Remaining items — standard style */
    s_menu_items[MENU_PARAMS]   = create_menu_item(menu_cont,
                                    LV_SYMBOL_LIST "  View Params", MENU_PARAMS, false);
    s_menu_items[MENU_SETTINGS] = create_menu_item(menu_cont,
                                    LV_SYMBOL_SETTINGS "  Settings", MENU_SETTINGS, false);
    s_menu_items[MENU_ABOUT]    = create_menu_item(menu_cont,
                                    LV_SYMBOL_HOME "  About", MENU_ABOUT, false);

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
    lv_label_set_text(hint_l, LV_SYMBOL_UP LV_SYMBOL_DOWN "  Scroll");  /* #8: verbs not labels */
    lv_obj_set_style_text_color(hint_l, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(hint_l, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_l, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *hint_r = lv_label_create(hbar);
    lv_label_set_text(hint_r, "OK  Select  " LV_SYMBOL_OK);
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

void scr_home_get_e_range(float *e_begin_mV, float *e_end_mV)
{
    if (e_begin_mV) *e_begin_mV = s_e_begin_mV;
    if (e_end_mV)   *e_end_mV   = s_e_end_mV;
}
