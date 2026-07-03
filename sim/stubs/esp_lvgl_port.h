/**
 * @file esp_lvgl_port.h  (PC simulator stub)
 * screen_mgr.c includes this for documentation only (no actual calls after
 * the P4 code-review fix). Provide an empty stub so it compiles.
 */
#pragma once
#include "lvgl.h"

/* Lock/unlock are no-ops in the single-threaded SDL sim */
static inline int  lvgl_port_lock(uint32_t timeout_ms)  { (void)timeout_ms; return 1; }
static inline void lvgl_port_unlock(void) {}
