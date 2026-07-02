#include "pstat_hal/pstat_hal.h"
#include "echem_core/calibration.h"
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hal_io";

static button_handle_t s_btn_start = NULL;
static button_handle_t s_btn_nav   = NULL;

/* ========== LEDs ========== */

esp_err_t pstat_led_init(void)
{
    /*
     * GPIO12 (READY LED) is a STRAPPING PIN (VDD_SDIO).
     * It is sampled at RESET — must NOT be HIGH at the moment of reset.
     * The LED-to-GND wiring idles this pin LOW, so normal boot is safe.
     */
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_UBIOPOT_LED_READY) |
                        (1ULL << CONFIG_UBIOPOT_LED_PROCESSING),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(CONFIG_UBIOPOT_LED_READY,      0);
    gpio_set_level(CONFIG_UBIOPOT_LED_PROCESSING, 0);
    ESP_LOGI(TAG, "LEDs init OK — READY=GPIO%d, PROCESSING=GPIO%d",
             CONFIG_UBIOPOT_LED_READY, CONFIG_UBIOPOT_LED_PROCESSING);
    return ESP_OK;
}

esp_err_t pstat_led_set(pstat_led_t led, bool on)
{
    int gpio = (led == PSTAT_LED_READY)
                   ? CONFIG_UBIOPOT_LED_READY
                   : CONFIG_UBIOPOT_LED_PROCESSING;
    gpio_set_level(gpio, on ? 1 : 0);
    return ESP_OK;
}

/* ========== Buttons (iot_button v4.x API) ========== */
/*
 * v4.x changes vs v3.x:
 *   - button_config_t now only holds timing (no .type / .gpio_button_config)
 *   - GPIO button created with iot_button_new_gpio_device(btn_cfg, gpio_cfg, &handle)
 *   - iot_button_register_cb takes 5 args: (handle, event, event_args, cb, usr_data)
 *     event_args = NULL for click/press; set long_press.press_time for LONG_PRESS_START
 *   - button_cb_t = void(*)(void *button_handle, void *usr_data)  [two args]
 *
 * START button (GPIO14): short press = select/start, long press >=2 s = abort (P4).
 * NAV   button (GPIO0) : short press = navigate, long press >=10 s = factory reset.
 *   GPIO0 is a STRAPPING PIN — must be HIGH at boot. Safe for runtime nav.
 */

esp_err_t pstat_buttons_init(void)
{
    /* -- START button -- */
    button_config_t btn_cfg_start = {
        .long_press_time  = 2000,  /* 2 s = abort scan (P4 wires this) */
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg_start = {
        .gpio_num          = CONFIG_UBIOPOT_BTN_START,
        .active_level      = 0,    /* active-low */
        .enable_power_save = false,
        .disable_pull      = false, /* use internal pull-up (INPUT_PULLUP) */
    };
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg_start, &gpio_cfg_start, &s_btn_start);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device(START GPIO%d) failed: %s",
                 CONFIG_UBIOPOT_BTN_START, esp_err_to_name(ret));
        return ret;
    }

    /* -- NAV/factory-reset button -- */
    button_config_t btn_cfg_nav = {
        .long_press_time  = 10000, /* 10 s = factory reset */
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg_nav = {
        .gpio_num          = CONFIG_UBIOPOT_BTN_NAV,
        .active_level      = 0,
        .enable_power_save = false,
        .disable_pull      = false,
    };
    ret = iot_button_new_gpio_device(&btn_cfg_nav, &gpio_cfg_nav, &s_btn_nav);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device(NAV GPIO%d) failed: %s",
                 CONFIG_UBIOPOT_BTN_NAV, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Buttons init OK — START=GPIO%d, NAV=GPIO%d (strapping: HIGH at boot)",
             CONFIG_UBIOPOT_BTN_START, CONFIG_UBIOPOT_BTN_NAV);
    return ESP_OK;
}

esp_err_t pstat_button_start_on_click(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_start) return ESP_ERR_INVALID_STATE;
    /* event_args = NULL → fires on every single click */
    return iot_button_register_cb(s_btn_start, BUTTON_SINGLE_CLICK, NULL,
                                  (button_cb_t)cb, arg);
}

esp_err_t pstat_button_nav_on_click(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_nav) return ESP_ERR_INVALID_STATE;
    return iot_button_register_cb(s_btn_nav, BUTTON_SINGLE_CLICK, NULL,
                                  (button_cb_t)cb, arg);
}

esp_err_t pstat_button_nav_on_factory_reset(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_nav) return ESP_ERR_INVALID_STATE;
    /*
     * Use BUTTON_LONG_PRESS_START with event_args.long_press.press_time = 10 000 ms.
     * This fires once when the 10 s threshold is reached (not every tick like HOLD).
     */
    button_event_args_t event_args = {
        .long_press.press_time = 10000,
    };
    return iot_button_register_cb(s_btn_nav, BUTTON_LONG_PRESS_START, &event_args,
                                  (button_cb_t)cb, arg);
}

/* ========== Self-test (P1 bringup) ========== */

/* Selftest logging callbacks — match v4.x button_cb_t signature */
static void selftest_btn_start_cb(void *handle, void *usr_data)
{
    (void)handle; (void)usr_data;
    ESP_LOGI("hal_selftest", "START button pressed (GPIO%d)", CONFIG_UBIOPOT_BTN_START);
}

static void selftest_btn_nav_cb(void *handle, void *usr_data)
{
    (void)handle; (void)usr_data;
    ESP_LOGI("hal_selftest", "NAV button pressed (GPIO%d)", CONFIG_UBIOPOT_BTN_NAV);
}

esp_err_t pstat_hal_selftest(const pstat_calib_t *cal)
{
    ESP_LOGI(TAG, "======= HAL selftest START =======");
    bool pass = true;

    /* 1. LED blink */
    ESP_LOGI(TAG, "[1/4] LED blink");
    for (int i = 0; i < 3; i++) {
        pstat_led_set(PSTAT_LED_READY,      true);
        vTaskDelay(pdMS_TO_TICKS(200));
        pstat_led_set(PSTAT_LED_READY,      false);
        pstat_led_set(PSTAT_LED_PROCESSING, true);
        vTaskDelay(pdMS_TO_TICKS(200));
        pstat_led_set(PSTAT_LED_PROCESSING, false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 2. DAC ramp sweep */
    ESP_LOGI(TAG, "[2/4] DAC ramp sweep");
    for (uint16_t code = 0; code <= 4095u; code += 512u) {
        esp_err_t ret = pstat_dac_set_code(code);
        float v = calib_dac_to_volt(code, cal);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  DAC code=%4u  Vout_expected=%.3f V", code, (double)v);
        } else {
            ESP_LOGE(TAG, "  DAC set_code(%u) FAILED: %s", code, esp_err_to_name(ret));
            pass = false;
        }
    }
    pstat_dac_set_volt(0.0f, cal);

    /* 3. ADC reads */
    ESP_LOGI(TAG, "[3/4] ADC reads (10 samples)");
    for (int i = 0; i < 10; i++) {
        float I = pstat_adc_read_current_uA(1, cal);
        float V = pstat_adc_read_cell_volt(cal);
        ESP_LOGI(TAG, "  sample %2d: I=%.3f uA  V=%.4f V", i, (double)I, (double)V);
    }

    /* 4. Mux cycle */
    ESP_LOGI(TAG, "[4/4] Mux cycle T1->T2->T3->off");
    for (uint8_t e = 1; e <= 3; e++) {
        pstat_mux_select(e);
        ESP_LOGI(TAG, "  electrode %u selected", e);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pstat_mux_deselect_all();

    /* Register temporary logging callbacks for button test */
    if (s_btn_start) {
        iot_button_register_cb(s_btn_start, BUTTON_SINGLE_CLICK, NULL,
                               selftest_btn_start_cb, NULL);
    }
    if (s_btn_nav) {
        iot_button_register_cb(s_btn_nav, BUTTON_SINGLE_CLICK, NULL,
                               selftest_btn_nav_cb, NULL);
    }

    if (pass) {
        pstat_led_set(PSTAT_LED_READY, true);
        ESP_LOGI(TAG, "======= HAL selftest PASSED — press buttons within 10 s =======");
    } else {
        pstat_led_set(PSTAT_LED_PROCESSING, true);
        ESP_LOGE(TAG, "======= HAL selftest FAILED =======");
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "Button wait done.");

    return pass ? ESP_OK : ESP_FAIL;
}

/* ========== Convenience init ========== */

esp_err_t pstat_hal_init_all(void)
{
    esp_err_t ret;
    if ((ret = pstat_dac_init())     != ESP_OK) { ESP_LOGE(TAG, "pstat_dac_init: %s",     esp_err_to_name(ret)); return ret; }
    if ((ret = pstat_adc_init())     != ESP_OK) { ESP_LOGE(TAG, "pstat_adc_init: %s",     esp_err_to_name(ret)); return ret; }
    if ((ret = pstat_mux_init())     != ESP_OK) { ESP_LOGE(TAG, "pstat_mux_init: %s",     esp_err_to_name(ret)); return ret; }
    if ((ret = pstat_led_init())     != ESP_OK) { ESP_LOGE(TAG, "pstat_led_init: %s",     esp_err_to_name(ret)); return ret; }
    if ((ret = pstat_buttons_init()) != ESP_OK) { ESP_LOGE(TAG, "pstat_buttons_init: %s", esp_err_to_name(ret)); return ret; }
    ESP_LOGI(TAG, "pstat_hal_init_all OK");
    return ESP_OK;
}
