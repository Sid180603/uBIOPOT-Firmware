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
    lv_label_set_text(top, "BITS Pilani   Aqua-HMET");
    lv_obj_set_style_text_color(top, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(top, &lv_font_montserrat_14, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 10);

    /* ── Main title ─────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Aqua-HMET");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    /* ── Sub-label ──────────────────────────────────────────────── */
    lv_obj_t *sub = lv_label_create(s_scr);
    lv_label_set_text(sub, "DPV Heavy Metal Detector");
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_DIM), 0);  /* #5: dim subtitle — title dominates */
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 14);

    /* ── Version tag ────────────────────────────────────────────── */
    lv_obj_t *ver = lv_label_create(s_scr);
    lv_label_set_text(ver, "fw v1.0.0  |  DPV");
    lv_obj_set_style_text_color(ver, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* ── Cascading fade-in animations (#14) ────────────────────────
     * Stagger: top@0ms(300ms), title@200ms(800ms, existing),
     *          sub@800ms(400ms), ver@1200ms(400ms).
     * User's eye follows top→title→sub→ver = top-down hierarchy.    */

    /* Helper macro: set initial opacity TRANSP so objects are invisible */
    lv_obj_set_style_opa(top,   LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(sub,   LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(ver,   LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(title, LV_OPA_TRANSP, 0);

    /* Top label: delay 0 ms, 300 ms */
    lv_anim_t a_top;
    lv_anim_init(&a_top);
    lv_anim_set_var(&a_top, top);
    lv_anim_set_exec_cb(&a_top, splash_set_opa);
    lv_anim_set_values(&a_top, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_top, 300);
    lv_anim_set_delay(&a_top, 0);
    lv_anim_start(&a_top);

    /* Main title: delay 200 ms, 800 ms */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, title);
    lv_anim_set_exec_cb(&a, splash_set_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 800);
    lv_anim_set_delay(&a, 200);
    lv_anim_start(&a);

    /* Subtitle: delay 800 ms, 400 ms */
    lv_anim_t a_sub;
    lv_anim_init(&a_sub);
    lv_anim_set_var(&a_sub, sub);
    lv_anim_set_exec_cb(&a_sub, splash_set_opa);
    lv_anim_set_values(&a_sub, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_sub, 400);
    lv_anim_set_delay(&a_sub, 800);
    lv_anim_start(&a_sub);

    /* Version tag: delay 1200 ms, 400 ms */
    lv_anim_t a_ver;
    lv_anim_init(&a_ver);
    lv_anim_set_var(&a_ver, ver);
    lv_anim_set_exec_cb(&a_ver, splash_set_opa);
    lv_anim_set_values(&a_ver, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_ver, 400);
    lv_anim_set_delay(&a_ver, 1200);
    lv_anim_start(&a_ver);

    /* ── Auto-transition timer: go to Home after 2200 ms ────────── */
    lv_timer_create(splash_timer_cb, 2200, NULL);

    ESP_LOGI(TAG, "Splash screen created");
    return s_scr;
}

static void splash_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    /* BUG 3 fix: only transition if splash is still the active screen.
     * Prevents a stale timer from overriding navigation that happened
     * while the animation was in flight (e.g. if splash is ever re-entered
     * or the delay is shortened in future). */
    if (lv_scr_act() == s_scr) {
        screen_mgr_goto_home();
    }
}
