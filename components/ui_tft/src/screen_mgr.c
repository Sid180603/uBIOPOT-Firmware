/**
 * @file screen_mgr.c
 * @brief Screen manager — lifecycle, transitions, and theme application.
 *
 * All public functions that touch LVGL objects must be called under lvgl_port_lock().
 * Functions starting with scr_xxx_goto / screen_mgr_show_toast acquire the lock
 * internally, so callers do NOT need to hold it.
 */

#include "screen_mgr.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "screen_mgr";

/* -------------------------------------------------------------------------
 * Forward declarations for create functions (defined in each scr_*.c)
 * ------------------------------------------------------------------------- */

lv_obj_t *scr_splash_create(lv_group_t *group);
lv_obj_t *scr_home_create(lv_group_t *group);
lv_obj_t *scr_scan_create(lv_group_t *group);
lv_obj_t *scr_results_create(lv_group_t *group);
lv_obj_t *scr_settings_create(lv_group_t *group);

/* Defined in scr_toast.c */
void scr_toast_show(const char *msg);

/* -------------------------------------------------------------------------
 * Screen handles (created once, loaded on demand)
 * ------------------------------------------------------------------------- */

static lv_obj_t *s_scr_splash   = NULL;
static lv_obj_t *s_scr_home     = NULL;
static lv_obj_t *s_scr_scan     = NULL;
static lv_obj_t *s_scr_results  = NULL;
static lv_obj_t *s_scr_settings = NULL;

static lv_group_t   *s_group    = NULL;
static lv_display_t *s_disp     = NULL;

/* -------------------------------------------------------------------------
 * screen_mgr_init — called once inside lvgl_port_lock()
 * ------------------------------------------------------------------------- */

void screen_mgr_init(lv_display_t *disp, lv_group_t *group)
{
    s_disp  = disp;
    s_group = group;

    /* Create all screens */
    s_scr_splash   = scr_splash_create(group);
    s_scr_home     = scr_home_create(group);
    s_scr_scan     = scr_scan_create(group);
    s_scr_results  = scr_results_create(group);
    s_scr_settings = scr_settings_create(group);

    /* Show splash first */
    lv_disp_load_scr(s_scr_splash);

    ESP_LOGI(TAG, "All screens created, splash loaded");
}

/* -------------------------------------------------------------------------
 * Transition helpers
 * ------------------------------------------------------------------------- */

static void load_screen(lv_obj_t *scr, lv_scr_load_anim_t anim)
{
    if (!scr) return;
    /* Switch the encoder group focus to the new screen's first focusable object */
    if (s_group) lv_group_focus_next(s_group);
    lv_scr_load_anim(scr, anim, 200, 0, false);
}

/* -------------------------------------------------------------------------
 * Public — screen transitions (acquire lock internally)
 * ------------------------------------------------------------------------- */

void screen_mgr_goto_home(void)
{
    if (!lvgl_port_lock(0)) return;
    load_screen(s_scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    lvgl_port_unlock();
}

void screen_mgr_goto_scan(void)
{
    if (!lvgl_port_lock(0)) return;
    load_screen(s_scr_scan, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    lvgl_port_unlock();
}

void screen_mgr_goto_results(void)
{
    if (!lvgl_port_lock(0)) return;
    load_screen(s_scr_results, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    lvgl_port_unlock();
}

void screen_mgr_goto_settings(void)
{
    if (!lvgl_port_lock(0)) return;
    load_screen(s_scr_settings, LV_SCR_LOAD_ANIM_OVER_LEFT);
    lvgl_port_unlock();
}

void screen_mgr_show_toast(const char *msg)
{
    if (!lvgl_port_lock(0)) return;
    scr_toast_show(msg);
    lvgl_port_unlock();
}

/* -------------------------------------------------------------------------
 * Public — scan-live updates (called from engine sink — already under lock)
 * ------------------------------------------------------------------------- */

/* These forward directly to the scan screen's internal functions */
/* (Implemented in scr_scan.c; declared in screen_mgr.h)          */
