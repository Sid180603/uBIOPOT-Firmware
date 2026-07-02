#include "pstat_hal/pstat_hal.h"
#include "esp_log.h"
/* TODO P1: #include "driver/gpio.h" */
/* TODO P1: #include "iot_button.h"  (espressif/button, pulled by esp_lvgl_port) */

static const char *TAG = "hal_io";

/*
 * TODO P1: GPIO LEDs + iot_button debounced button driver.
 *
 * LEDs:
 *   GPIO12 = READY LED (D2, R19=100Ω). STRAPPING PIN: must NOT be HIGH at reset.
 *   GPIO13 = PROCESSING LED (D1, R18=100Ω).
 *   Both configured as OUTPUT, initial level LOW.
 *
 *   LED drive convention: HIGH = LED ON (current flows through resistor to LED anode → cathode → GND).
 *   At boot GPIO12 is sampled for VDD_SDIO voltage selection. With LED-to-GND wiring
 *   the pin idles LOW, so boot is safe. Never drive GPIO12 HIGH in a reset handler or
 *   before the strapping window (~100 ms after EN rises) — covered by our pstat_led_init()
 *   being called well into app_main, not at early boot.
 *
 * Buttons (via iot_button, espressif/button):
 *   GPIO14 = START (active-low, INPUT_PULLUP). Clean GPIO, no strapping concerns.
 *   GPIO0  = NAV/factory-reset (active-low, INPUT_PULLUP). STRAPPING/BOOT pin.
 *            Must be HIGH at power-on. Safe to read at runtime.
 *            Long hold ≥10 s → factory reset callback.
 *
 * P1 implementation:
 *   1. gpio_config() for both LEDs.
 *   2. iot_button_create() for GPIO14 and GPIO0 (active_level=0).
 *   3. iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, cb, arg) for click callbacks.
 *   4. For factory reset: iot_button_register_cb(btn0, BUTTON_LONG_PRESS_START, ...) with
 *      BUTTON_LONG_PRESS_TIME_MS = 10000.
 *
 * Note: iot_button supports multiple callbacks per button instance, so short-click and
 * long-press coexist cleanly on GPIO0 (used for both LVGL navigation and factory reset).
 */

esp_err_t pstat_led_init(void)
{
    ESP_LOGW(TAG, "stub — not implemented (P1)");
    return ESP_OK;
}

esp_err_t pstat_led_set(pstat_led_t led, bool on)
{
    (void)led;
    (void)on;
    return ESP_OK;
}

esp_err_t pstat_buttons_init(void)
{
    ESP_LOGW(TAG, "stub — not implemented (P1)");
    return ESP_OK;
}

esp_err_t pstat_button_start_on_click(pstat_button_cb_t cb, void *arg)
{
    (void)cb;
    (void)arg;
    return ESP_OK;
}

esp_err_t pstat_button_nav_on_click(pstat_button_cb_t cb, void *arg)
{
    (void)cb;
    (void)arg;
    return ESP_OK;
}

esp_err_t pstat_button_nav_on_factory_reset(pstat_button_cb_t cb, void *arg)
{
    (void)cb;
    (void)arg;
    return ESP_OK;
}

esp_err_t pstat_hal_selftest(const pstat_calib_t *cal)
{
    (void)cal;
    ESP_LOGW(TAG, "hal_selftest stub — not implemented (P1)");
    return ESP_OK;
}
