#include "pstat_hal/pstat_hal.h"
#include "echem_core/calibration.h"
#include "driver/gpio.h"
#include "iot_button.h"
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
     * The firmware must never drive GPIO12 HIGH inside a reset/watchdog handler.
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

/* ========== Buttons ========== */

esp_err_t pstat_buttons_init(void)
{
    /*
     * START button (GPIO14): active-low, INPUT_PULLUP.
     *   Short press = select/start DPV.
     *   Long press  = abort scan (wired in P4 via pstat_button_start_on_long_press).
     *
     * NAV/factory-reset button (GPIO0): active-low.
     *   STRAPPING PIN: must be HIGH at boot (do not hold during reset).
     *   Short press = navigate/rotate LVGL encoder focus.
     *   Long hold ≥ 10 s = factory reset WiFi creds (registered below).
     */
    button_config_t cfg_start = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = 2000,  /* 2 s hold = abort (P4 wires this) */
        .short_press_time = 50,
        .gpio_button_config = {
            .gpio_num    = CONFIG_UBIOPOT_BTN_START,
            .active_level = 0,
        },
    };
    s_btn_start = iot_button_create(&cfg_start);
    if (!s_btn_start) {
        ESP_LOGE(TAG, "iot_button_create(START GPIO%d) failed", CONFIG_UBIOPOT_BTN_START);
        return ESP_FAIL;
    }

    button_config_t cfg_nav = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = 10000, /* 10 s = factory reset */
        .short_press_time = 50,
        .gpio_button_config = {
            .gpio_num    = CONFIG_UBIOPOT_BTN_NAV,
            .active_level = 0,
        },
    };
    s_btn_nav = iot_button_create(&cfg_nav);
    if (!s_btn_nav) {
        ESP_LOGE(TAG, "iot_button_create(NAV GPIO%d) failed", CONFIG_UBIOPOT_BTN_NAV);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Buttons init OK — START=GPIO%d, NAV=GPIO%d (strapping: HIGH at boot)",
             CONFIG_UBIOPOT_BTN_START, CONFIG_UBIOPOT_BTN_NAV);
    return ESP_OK;
}

esp_err_t pstat_button_start_on_click(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_start) return ESP_ERR_INVALID_STATE;
    return iot_button_register_cb(s_btn_start, BUTTON_SINGLE_CLICK, (button_cb_t)cb, arg);
}

esp_err_t pstat_button_nav_on_click(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_nav) return ESP_ERR_INVALID_STATE;
    return iot_button_register_cb(s_btn_nav, BUTTON_SINGLE_CLICK, (button_cb_t)cb, arg);
}

esp_err_t pstat_button_nav_on_factory_reset(pstat_button_cb_t cb, void *arg)
{
    if (!s_btn_nav) return ESP_ERR_INVALID_STATE;
    return iot_button_register_cb(s_btn_nav, BUTTON_LONG_PRESS_START, (button_cb_t)cb, arg);
}

/* ========== Self-test (P1 bringup) ========== */

esp_err_t pstat_hal_selftest(const pstat_calib_t *cal)
{
    ESP_LOGI(TAG, "======= HAL selftest START =======");
    bool pass = true;

    /* 1. LED blink (visual) */
    ESP_LOGI(TAG, "[1/4] LED blink test");
    for (int i = 0; i < 3; i++) {
        pstat_led_set(PSTAT_LED_READY,      true);
        vTaskDelay(pdMS_TO_TICKS(200));
        pstat_led_set(PSTAT_LED_READY,      false);
        pstat_led_set(PSTAT_LED_PROCESSING, true);
        vTaskDelay(pdMS_TO_TICKS(200));
        pstat_led_set(PSTAT_LED_PROCESSING, false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "[1/4] LED blink done (check board visually)");

    /* 2. DAC ramp sweep */
    ESP_LOGI(TAG, "[2/4] DAC ramp sweep (0..4095 in 512 steps)");
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
    pstat_dac_set_volt(0.0f, cal);  /* park at 0 V */

    /* 3. ADC reads (10 samples from AIN1 and AIN0) */
    ESP_LOGI(TAG, "[3/4] ADC reads (10 samples each channel)");
    for (int i = 0; i < 10; i++) {
        float I = pstat_adc_read_current_uA(1, cal);
        float V = pstat_adc_read_cell_volt(cal);
        ESP_LOGI(TAG, "  sample %2d: I=%.3f uA  V=%.4f V", i, (double)I, (double)V);
    }

    /* 4. Mux cycle */
    ESP_LOGI(TAG, "[4/4] Mux cycle (T1 -> T2 -> T3 -> off)");
    for (uint8_t e = 1; e <= 3; e++) {
        pstat_mux_select(e);
        ESP_LOGI(TAG, "  electrode %u selected", e);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pstat_mux_deselect_all();

    /* Results */
    if (pass) {
        pstat_led_set(PSTAT_LED_READY, true);
        ESP_LOGI(TAG, "======= HAL selftest PASSED =======");
    } else {
        pstat_led_set(PSTAT_LED_PROCESSING, true);
        ESP_LOGE(TAG, "======= HAL selftest FAILED =======");
    }

    /* Buttons: wait 10 s for press events (logged via registered callbacks) */
    ESP_LOGI(TAG, "Press START or NAV button within 10 s to verify callbacks...");
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "Button wait complete.");

    return pass ? ESP_OK : ESP_FAIL;
}

/* ========== Convenience init ========== */

esp_err_t pstat_hal_init_all(void)
{
    esp_err_t ret;

    ret = pstat_dac_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_dac_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pstat_adc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_adc_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pstat_mux_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_mux_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pstat_led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_led_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pstat_buttons_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_buttons_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "pstat_hal_init_all OK");
    return ESP_OK;
}
