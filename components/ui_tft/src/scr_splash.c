/**
 * @file scr_splash.c
 * @brief Splash screen — logo + version, fade-in, auto-transitions to Home after 2 s.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "scr_splash";

static lv_obj_t *s_scr = NULL;

/* Forward declaration */
static void splash_timer_cb(lv_timer_t *t);

/* -------------------------------------------------------------------------
 * Helpers — dark-themed label creator
 * ------------------------------------------------------------------------- */

static void apply_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

/* Wrapper: animates object opacity without the 3-arg style function cast */
static void splash_set_opa(void *obj, int32_t val)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

/* -------------------------------------------------------------------------
 * Public
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_splash_create(lv_group_t *group)
{
    (void)group; /* Splash has no focusable objects */

    s_scr = lv_obj_create(NULL);
    apply_bg(s_scr);
    lv_obj_set_size(s_scr, LV_HOR_RES, LV_VER_RES);

    /* ── Top micro-label ────────────────────────────────────────── */
    lv_obj_t *top = lv_label_create(s_scr);
    lv_label_set_text(top, "BITS Pilani   uBIOPOT");
    lv_obj_set_style_text_color(top, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(top, &lv_font_montserrat_14, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 10);

    /* ── Main title ─────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "uBIOPOT");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    /* ── Sub-label ──────────────────────────────────────────────── */
    lv_obj_t *sub = lv_label_create(s_scr);
    lv_label_set_text(sub, "Multiplexed Potentiostat v2");
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 14);

    /* ── Version tag ────────────────────────────────────────────── */
    lv_obj_t *ver = lv_label_create(s_scr);
    lv_label_set_text(ver, "fw v2.0.0  |  DPV");
    lv_obj_set_style_text_color(ver, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* ── Fade-in animation on the main title ────────────────────── */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, title);
    lv_anim_set_exec_cb(&a, splash_set_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 800);
    lv_anim_start(&a);

    /* ── Auto-transition timer: go to Home after 2200 ms ────────── */
    lv_timer_create(splash_timer_cb, 2200, NULL);

    ESP_LOGI(TAG, "Splash screen created");
    return s_scr;
}

static void splash_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    screen_mgr_goto_home();
}
