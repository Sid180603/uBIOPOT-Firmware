/**
 * @file screen_mgr.c
 * @brief Screen manager — lifecycle, transitions, and theme application.
 *
 * All public functions (transitions, updates) MUST be called under lvgl_port_lock().
 * They do NOT acquire the lock themselves — this avoids double-locking when called
 * from the engine sink (which holds the lock) or from LVGL event callbacks (which run
 * inside lv_timer_handler under the LVGL recursive mutex).
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

/* One lv_group per screen — encoder focus is scoped to the active screen.
 * Per-screen groups prevent lv_group_focus_next from jumping to objects on
 * a screen that is no longer visible after a transition. */
static lv_group_t *s_grp_splash   = NULL;
static lv_group_t *s_grp_home     = NULL;
static lv_group_t *s_grp_scan     = NULL;
static lv_group_t *s_grp_results  = NULL;
static lv_group_t *s_grp_settings = NULL;

static lv_indev_t   *s_indev = NULL;
static lv_display_t *s_disp  = NULL;

/* -------------------------------------------------------------------------
 * screen_mgr_init — called once inside lvgl_port_lock()
 * ------------------------------------------------------------------------- */

void screen_mgr_init(lv_display_t *disp, lv_indev_t *indev)
{
    s_disp  = disp;
    s_indev = indev;

    /* Create one group per screen so encoder focus stays on visible objects. */
    s_grp_splash   = lv_group_create();
    s_grp_home     = lv_group_create();
    s_grp_scan     = lv_group_create();
    s_grp_results  = lv_group_create();
    s_grp_settings = lv_group_create();

    /* Create all screens, each receiving its own group. */
    s_scr_splash   = scr_splash_create(s_grp_splash);
    s_scr_home     = scr_home_create(s_grp_home);
    s_scr_scan     = scr_scan_create(s_grp_scan);
    s_scr_results  = scr_results_create(s_grp_results);
    s_scr_settings = scr_settings_create(s_grp_settings);

    /* Start on splash; bind indev to splash group initially. */
    lv_indev_set_group(s_indev, s_grp_splash);
    lv_disp_load_scr(s_scr_splash);

    ESP_LOGI(TAG, "All screens created, splash loaded");
}

/* -------------------------------------------------------------------------
 * Transition helpers
 * ------------------------------------------------------------------------- */

static void load_screen(lv_obj_t *scr, lv_scr_load_anim_t anim)
{
    if (!scr) return;
    /* Switch every non-pointer indev to the group for the incoming screen.
     * - On firmware: updates the encoder indev (GPIO14/GPIO0).
     * - On PC sim:   updates both the mousewheel encoder AND the SDL keyboard,
     *                so Tab/Enter navigation works on every screen. */
    lv_group_t *g = NULL;
    if      (scr == s_scr_home)     g = s_grp_home;
    else if (scr == s_scr_scan)     g = s_grp_scan;
    else if (scr == s_scr_results)  g = s_grp_results;
    else if (scr == s_scr_settings) g = s_grp_settings;
    else if (scr == s_scr_splash)   g = s_grp_splash;

    if (g) {
        for (lv_indev_t *iv = lv_indev_get_next(NULL); iv != NULL;
             iv = lv_indev_get_next(iv)) {
            if (lv_indev_get_type(iv) != LV_INDEV_TYPE_POINTER) {
                lv_indev_set_group(iv, g);
            }
        }
    }
    lv_scr_load_anim(scr, anim, 200, 0, false);
}

/* -------------------------------------------------------------------------
 * Public — screen transitions (acquire lock internally)
 * ------------------------------------------------------------------------- */

/* All callers (LVGL event callbacks and engine sink) already hold lvgl_port_lock(). */

void screen_mgr_goto_home(void)
{
    load_screen(s_scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

void screen_mgr_goto_scan(void)
{
    load_screen(s_scr_scan, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

void screen_mgr_goto_results(void)
{
    load_screen(s_scr_results, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

void screen_mgr_goto_settings(void)
{
    load_screen(s_scr_settings, LV_SCR_LOAD_ANIM_OVER_LEFT);
}

void screen_mgr_show_toast(const char *msg)
{
    scr_toast_show(msg);
}

/* -------------------------------------------------------------------------
 * Public — scan-live updates (called from engine sink — already under lock)
 * ------------------------------------------------------------------------- */

/* These forward directly to the scan screen's internal functions */
/* (Implemented in scr_scan.c; declared in screen_mgr.h)          */
