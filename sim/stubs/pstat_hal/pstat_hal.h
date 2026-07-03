/**
 * @file pstat_hal/pstat_hal.h  (PC simulator stub)
 *
 * Provides the minimum type definitions and function declarations needed by
 * the screen files so they compile on the host without the real ESP-IDF HAL.
 *
 * Signatures MUST match pstat_hal/include/pstat_hal/pstat_hal.h exactly to
 * prevent redefinition errors if the real header is ever pulled in alongside.
 */
#pragma once

#include "esp_err.h"   /* esp_err_t */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- LED identifiers (mirrors pstat_led_t enum in real HAL) --------- */
typedef enum {
    PSTAT_LED_READY      = 0,
    PSTAT_LED_PROCESSING = 1,
} pstat_led_t;

/**
 * Set a status LED on or off.
 * Stub: no-op on PC (no GPIO). Returns ESP_OK.
 * Signature matches the real firmware declaration.
 */
esp_err_t pstat_led_set(pstat_led_t led, bool on);

#ifdef __cplusplus
}
#endif
