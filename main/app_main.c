#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ubiopot";

void app_main(void)
{
    ESP_LOGI(TAG, "uBIOPOT v2 starting — P0 skeleton (nothing functional yet)");

    /*
     * Startup sequence (filled in phase by phase):
     *
     * P2: technique_registry_init()            — register DPV (and stubs for CV/LSV/etc)
     * P1: pstat_hal_init()                     — init MCP4921 DAC, ADS1115 ADC, CD4066 mux,
     *                                            buttons (GPIO14/GPIO0), LEDs (GPIO12/GPIO13)
     * P8: settings_load()                      — load calibration + WiFi creds from NVS
     * P3: acq_engine_start()                   — spawn AcquisitionTask (Core-1, high prio)
     *                                            + Dispatcher task (Core-0)
     * P4: ui_tft_start()                       — init ILI9341 + LVGL + 2-button encoder nav
     * P5: net_comms_start()                    — WiFi APSTA + captive portal + mDNS +
     *                                            HTTP server + WebSocket sink
     * P7: serial_comms_start()                 — UART0 NDJSON sink + RX command task
     *
     * After all inits: engine idles on Core-1, waiting for a START command from any sink
     * (TFT button press, WS {"cmd":"start",...}, or serial {"cmd":"start",...}).
     */

    /* Spin — all work happens in FreeRTOS tasks created above. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
