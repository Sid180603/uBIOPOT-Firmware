#include "pstat_hal/pstat_hal.h"
#include "esp_log.h"
/* TODO P1: #include "driver/gpio.h" */

static const char *TAG = "hal_mux";

/*
 * TODO P1: CD4066 electrode multiplexer driver.
 *
 * T1=GPIO32 (Electrode 1, ELECT_1), T2=GPIO25 (Electrode 2, ELECT_2), T3=GPIO33 (Electrode 3, ELECT_3).
 * Only ONE line HIGH at a time — multiple-HIGH would connect multiple electrodes simultaneously
 * and corrupt measurements.
 *
 * P1 implementation:
 *   1. gpio_config() all three as OUTPUT, initial level LOW.
 *   2. pstat_mux_select(e): set only T[e] HIGH, others LOW.
 *   3. pstat_mux_deselect_all(): set all LOW.
 *
 * Pin assignments from Blynk+TFT firmware + OP07 schematic (verified):
 *   D32 → ELECT_1 → U3A CD4066 → Electrode 1 (T1)
 *   D25 → ELECT_2 → U3B CD4066 → Electrode 2 (T2)   [NOT the internal DAC — that path is dead]
 *   D33 → ELECT_3 → U3C CD4066 → Electrode 3 (T3)
 *
 * Note from plan: GPIO25 is used for T2 mux output, NOT the internal ESP32 DAC1.
 * The internal DAC path is architecturally dead on this board (GPIO26 = MCP4921 CS).
 */

esp_err_t pstat_mux_init(void)
{
    ESP_LOGW(TAG, "stub — not implemented (P1)");
    return ESP_OK;
}

esp_err_t pstat_mux_select(uint8_t electrode)
{
    (void)electrode;
    return ESP_OK;
}

esp_err_t pstat_mux_deselect_all(void)
{
    return ESP_OK;
}
