/**
 * @file scr_toast.c
 * @brief Error toast overlay — red slide-in banner from top, auto-dismisses after 3 s.
 *
 * The toast is NOT a full screen; it is an lv_obj layered on top of the current screen.
 * screen_mgr_show_toast() creates a new overlay object on lv_layer_top() each time.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "scr_toast";

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *toast;
    lv_obj_t *label;
} toast_ctx_t;

static void toast_dismiss_cb(lv_timer_t *t);

/* Wrapper: set Y position for lv_anim_exec_xcb_t */
static void toast_set_y(void *obj, int32_t val)
{
    lv_obj_set_y((lv_obj_t *)obj, val);
}

/* Deleted callback: delete the toast object after slide-out */
static void toast_anim_deleted_cb(lv_anim_t *a)
{
    lv_obj_del_async((lv_obj_t *)a->var);
}

/* ---------------------------------------------------------------------------
 * Public
 * --------------------------------------------------------------------------- */

void scr_toast_show(const char *msg, toast_level_t level)
{
    /* Create on the top layer so it overlays whichever screen is active */
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, LV_HOR_RES - 20, 44);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_radius(toast, 6, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);

    if (level == TOAST_ERROR) {
        /* Red background — reserved for real failures */
        lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COLOR_ABORT), 0);
        lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    } else {
        /* Informational: dark-surface bg + 3px left accent strip */
        lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(toast, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(toast, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(toast, 3, 0);
        lv_obj_set_style_border_opa(toast, LV_OPA_COVER, 0);
    }
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, -60);

    /* Message label */
    lv_obj_t *lbl = lv_label_create(toast);
    lv_label_set_text(lbl, msg ? msg : "Unknown error");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, LV_HOR_RES - 44);
    lv_obj_set_style_text_color(lbl, (level == TOAST_ERROR) ? lv_color_white()
                                                             : lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    /* ── Slide-in animation: y from -60 to +6 ──────────────────── */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, toast);
    lv_anim_set_exec_cb(&a, toast_set_y);
    lv_anim_set_values(&a, -60, 6);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* Auto-dismiss timer (3 s) */
    lv_timer_t *tmr = lv_timer_create(toast_dismiss_cb, 3000, toast);
    lv_timer_set_repeat_count(tmr, 1);

    ESP_LOGW(TAG, "Toast[%s]: %s", (level == TOAST_ERROR) ? "ERR" : "INF", msg ? msg : "(null)");
}

static void toast_dismiss_cb(lv_timer_t *t)
{
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(t);
    if (!toast) return;

    /* Slide out back up */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, toast);
    lv_anim_set_exec_cb(&a, toast_set_y);
    lv_anim_set_values(&a, 6, -60);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_deleted_cb(&a, toast_anim_deleted_cb);
    lv_anim_start(&a);
}
