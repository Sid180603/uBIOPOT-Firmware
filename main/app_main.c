#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pstat_hal/pstat_hal.h"
#include "echem_core/calibration.h"
#include "echem_core/technique.h"
#include "acq_engine.h"
#include "ui_tft.h"
#include "net_comms.h"

static const char *TAG = "aqua-hmet";

/* Calibration constants -- loaded from NVS in P8; compile defaults for P0-P7. */
static pstat_calib_t s_cal = PSTAT_CALIB_DEFAULT;

void app_main(void)
{
    ESP_LOGI(TAG, "Aqua-HMET v1 starting");

    /* ---- P2: register DPV technique (and stubs for CV/LSV/SWV/NPV) ---- */
    technique_registry_init();

    /* ---- P1: initialise all HAL subsystems ---- */
    esp_err_t ret = pstat_hal_init_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_hal_init_all FAILED: %s -- halting", esp_err_to_name(ret));
        /* Blink PROCESSING LED rapidly to signal fatal init failure */
        for (;;) {
            pstat_led_set(PSTAT_LED_PROCESSING, true);
            vTaskDelay(pdMS_TO_TICKS(100));
            pstat_led_set(PSTAT_LED_PROCESSING, false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* ---- P1: optional HAL self-test (enabled via menuconfig) ---- */
#if CONFIG_UBIOPOT_SELFTEST_MODE
    ESP_LOGI(TAG, "SELFTEST mode -- running pstat_hal_selftest, then halting");
    pstat_calib_t cal = PSTAT_CALIB_DEFAULT;
    esp_err_t st = pstat_hal_selftest(&cal);
    ESP_LOGI(TAG, "pstat_hal_selftest: %s", st == ESP_OK ? "PASSED" : "FAILED");
    ESP_LOGI(TAG, "Selftest complete -- holding (reset to restart)");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
#endif

    /* Signal ready */
    pstat_led_set(PSTAT_LED_READY, true);
    pstat_led_set(PSTAT_LED_PROCESSING, false);
    ESP_LOGI(TAG, "Aqua-HMET v1 ready (READY LED on)");

    /* ---- P3: start acquisition engine (AcqTask Core-1 + DispTask Core-0) ---- */
    ret = acq_engine_init(&s_cal);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "acq_engine_init FAILED: %s", esp_err_to_name(ret));
        /* Non-fatal for now -- engine simply won't run scans. */
    }

    /*
     * Remaining startup (filled in phase by phase):
     * P8: settings_load(&s_cal) -- load calibration + WiFi creds from NVS
     * P4: ui_tft_start()        -- init ILI9341 + LVGL + 2-button encoder nav  ← DONE
     * P5: net_comms_start()     -- WiFi APSTA + captive portal + mDNS + HTTP + WebSocket ← DONE
     * P7: serial_comms_start()  -- UART0 NDJSON sink + RX command task
     */

    /* ---- P4: on-device TFT UI (ILI9341 + LVGL + encoder + engine sink) ---- */
    ret = ui_tft_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ui_tft_start FAILED: %s -- display will be blank", esp_err_to_name(ret));
        /* Non-fatal: device still runs scans via serial/WiFi (P5/P7) */
    }

    /* ---- NVS (required by WiFi for PHY calibration + credential storage) ---- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated — erasing and re-init");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init FAILED: %s", esp_err_to_name(ret));
    }

    /* ---- P5: WiFi APSTA + captive portal + mDNS + HTTP + WebSocket ---- */
    ret = net_comms_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "net_comms_start FAILED: %s -- WiFi unavailable", esp_err_to_name(ret));
        /* Non-fatal: TFT UI and serial (P7) still work */
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
