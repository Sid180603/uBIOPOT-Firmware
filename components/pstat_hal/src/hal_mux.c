#include "pstat_hal/pstat_hal.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "hal_mux";

/*
 * CD4066 electrode multiplexer.
 *
 * T1=GPIO32 (Electrode 1, ELECT_1), T2=GPIO25 (Electrode 2, ELECT_2),
 * T3=GPIO33 (Electrode 3, ELECT_3).
 *
 * INVARIANT: at most ONE line HIGH at a time.
 * Multiple-HIGH would connect several electrodes simultaneously → corrupt measurements.
 *
 * GPIO25 is used for T2 (ELECT_2) — NOT the internal ESP32 DAC.
 * Internal DAC was dropped (architecturally dead on OP07 board).
 */

esp_err_t pstat_mux_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_UBIOPOT_MUX_T1) |
                        (1ULL << CONFIG_UBIOPOT_MUX_T2) |
                        (1ULL << CONFIG_UBIOPOT_MUX_T3),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* All electrodes disconnected at init */
    pstat_mux_deselect_all();
    ESP_LOGI(TAG, "CD4066 mux init OK — T1=GPIO%d, T2=GPIO%d, T3=GPIO%d",
             CONFIG_UBIOPOT_MUX_T1, CONFIG_UBIOPOT_MUX_T2, CONFIG_UBIOPOT_MUX_T3);
    return ESP_OK;
}

esp_err_t pstat_mux_select(uint8_t electrode)
{
    /* First deselect all, then set the chosen line HIGH */
    gpio_set_level(CONFIG_UBIOPOT_MUX_T1, 0);
    gpio_set_level(CONFIG_UBIOPOT_MUX_T2, 0);
    gpio_set_level(CONFIG_UBIOPOT_MUX_T3, 0);

    switch (electrode) {
    case 1:  gpio_set_level(CONFIG_UBIOPOT_MUX_T1, 1); break;
    case 2:  gpio_set_level(CONFIG_UBIOPOT_MUX_T2, 1); break;
    case 3:  gpio_set_level(CONFIG_UBIOPOT_MUX_T3, 1); break;
    default:
        ESP_LOGE(TAG, "pstat_mux_select: invalid electrode %u (must be 1, 2, or 3)", electrode);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t pstat_mux_deselect_all(void)
{
    gpio_set_level(CONFIG_UBIOPOT_MUX_T1, 0);
    gpio_set_level(CONFIG_UBIOPOT_MUX_T2, 0);
    gpio_set_level(CONFIG_UBIOPOT_MUX_T3, 0);
    return ESP_OK;
}
