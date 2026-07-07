/**
 * @file serial_comms.c
 * @brief P7 — USB-serial NDJSON protocol implementation.
 *
 * Subsystem overview:
 *   1. UART0 init: 115200 baud (Kconfig), 8N1, GPIO1/TX GPIO3/RX, 4KB ring buffers.
 *   2. Log routing: esp_log_level_set("*", ESP_LOG_WARN) — suppress INFO/DEBUG on
 *      UART0 to avoid corrupting NDJSON stream.  Host ignores non-'{' lines.
 *   3. Engine sink registration + hello handshake.
 *   4. serial_rx_task: line accumulator → JSON command parser → engine API.
 *   5. Sink callbacks: snprintf-based NDJSON (hot path, avoids heap alloc);
 *      cJSON for multi-field event objects.
 *
 * TX mutex: all UART0 writes (point, event, resync, state reply) are
 * serialised via s_tx_mutex so no two NDJSON lines interleave, even if
 * serial_rx_task sends a state reply at the same time the Dispatcher
 * emits a DataPoint.
 *
 * RX line accumulator: reads chunks (up to 64 bytes per call), appends
 * to line buffer, processes on '\n' or '\r'.  Lines > SERIAL_CMD_LINE_MAX
 * are silently discarded (overflow protection).
 */

#include "serial_comms.h"
#include "serial_comms_protocol.h"

#include "acq_engine.h"
#include "echem_core/scan_state.h"
#include "echem_core/dpv.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "serial_comms";

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

#define SERIAL_UART_NUM      UART_NUM_0
#define SERIAL_RX_BUF_BYTES  4096
#define SERIAL_TX_BUF_BYTES  4096
/* SERIAL_CHUNK_BYTES is defined in serial_comms_protocol.h */
#define RX_TASK_STACK        3072
#define RX_TASK_PRIO         5       /* below Dispatcher (medium), above idle */

/* Baud rate: use Kconfig value if defined, else compile-time default */
#ifndef CONFIG_UBIOPOT_SERIAL_BAUD
#define CONFIG_UBIOPOT_SERIAL_BAUD  SERIAL_DEFAULT_BAUD
#endif

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_tx_mutex = NULL;
static engine_sink_t     s_serial_sink;

/* --------------------------------------------------------------------------
 * TX helper — sends one NDJSON line (json + '\n') under mutex
 * -------------------------------------------------------------------------- */

static void serial_tx_line(const char *json)
{
    if (!json || !s_tx_mutex) return;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout — drop line");
        return;
    }
    uart_write_bytes(SERIAL_UART_NUM, json, strlen(json));
    uart_write_bytes(SERIAL_UART_NUM, "\n", 1);
    xSemaphoreGive(s_tx_mutex);
}

/* --------------------------------------------------------------------------
 * Engine sink callbacks  (called from DispatcherTask, Core 0)
 * -------------------------------------------------------------------------- */

static void serial_on_point(const DataPoint *pt, void *ctx)
{
    (void)ctx;
    /* Use stack buffer + snprintf to avoid heap alloc in the hot path.
     * SERIAL_POINT_LINE_MAX (128) is safe for the longest possible line. */
    char buf[SERIAL_POINT_LINE_MAX];
    snprintf(buf, sizeof(buf),
        "{\"%s\":\"%s\",\"%s\":%u,\"%s\":%u,\"%s\":%.4f,\"%s\":%.4f,\"%s\":%.4f}",
        SERIAL_POINT_T,         SERIAL_T_POINT,
        SERIAL_POINT_ELECTRODE, (unsigned)pt->electrode,
        SERIAL_POINT_IDX,       (unsigned)pt->idx,
        SERIAL_POINT_E_MV,      (double)pt->E_mV,
        SERIAL_POINT_I_UA,      (double)pt->I_uA,
        SERIAL_POINT_RE_MV,     (double)pt->RE_mV);
    serial_tx_line(buf);
}

static void serial_on_event(scan_event_t evt, const char *info, void *ctx)
{
    (void)ctx;
    const char *name;
    switch (evt) {
        case SCAN_EVT_START:        name = SERIAL_EVT_NAME_STARTED;  break;
        case SCAN_EVT_EQUILIB_DONE: name = SERIAL_EVT_NAME_EQUILIB;  break;
        case SCAN_EVT_SCAN_DONE:    name = SERIAL_EVT_NAME_COMPLETE; break;
        case SCAN_EVT_ABORTED:      name = SERIAL_EVT_NAME_ABORTED;  break;
        case SCAN_EVT_ERROR:        name = SERIAL_EVT_NAME_ERROR;    break;
        case SCAN_EVT_RESET:        name = SERIAL_EVT_NAME_RESET;    break;
        default:                    name = "unknown";                 break;
    }

    cJSON *j = cJSON_CreateObject();
    if (!j) return;
    cJSON_AddStringToObject(j, "t",    SERIAL_T_EVENT);
    cJSON_AddStringToObject(j, "name", name);
    if (info && *info) {
        cJSON_AddStringToObject(j, "msg", info);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) {
        serial_tx_line(s);
        free(s);
    }
}

static void serial_on_resync(const DataPoint *buf, uint16_t count,
                              scan_state_t state, void *ctx)
{
    (void)ctx;
    /* Re-emit every buffered DataPoint (may be slow at 115200 for large scans,
     * but acceptable — serial resync is a diagnostic/attach operation). */
    for (uint16_t i = 0; i < count; i++) {
        serial_on_point(&buf[i], NULL);
    }
    /* Resync-complete summary */
    char line[96];
    snprintf(line, sizeof(line),
        "{\"t\":\"%s\",\"count\":%u,\"state\":%d}",
        SERIAL_T_RESYNC_COMPLETE, (unsigned)count, (int)state);
    serial_tx_line(line);
}

/* --------------------------------------------------------------------------
 * Command processing  (called from RX task with null-terminated JSON line)
 * -------------------------------------------------------------------------- */

static void serial_process_cmd(const char *line, int len)
{
    cJSON *j = cJSON_ParseWithLength(line, (size_t)len);
    if (!j) {
        ESP_LOGW(TAG, "RX: JSON parse fail: %.*s", len, line);
        return;
    }

    cJSON *cmd_j = cJSON_GetObjectItem(j, "cmd");
    const char *cmd = cmd_j ? cJSON_GetStringValue(cmd_j) : NULL;
    if (!cmd) {
        ESP_LOGW(TAG, "RX: missing 'cmd' field");
        cJSON_Delete(j);
        return;
    }

    if (strcmp(cmd, SERIAL_CMD_ABORT) == 0) {
        engine_abort();

    } else if (strcmp(cmd, SERIAL_CMD_ZERO) == 0) {
        engine_zero();

    } else if (strcmp(cmd, SERIAL_CMD_STATE) == 0) {
        char resp[64];
        snprintf(resp, sizeof(resp),
            "{\"t\":\"%s\",\"state\":%d}",
            SERIAL_T_STATE, (int)engine_get_state());
        serial_tx_line(resp);

    } else if (strcmp(cmd, SERIAL_CMD_HELLO) == 0) {
        /* Re-send hello and resync current scan state */
        char hello[128];
        snprintf(hello, sizeof(hello),
            "{\"t\":\"%s\",\"%s\":\"%s\",\"%s\":%d,\"%s\":\"%s\"}",
            SERIAL_T_HELLO,
            SERIAL_HELLO_FW,     SERIAL_FW_VERSION_STR,
            SERIAL_HELLO_PROTO,  SERIAL_PROTOCOL_VERSION,
            SERIAL_HELLO_DEVICE, SERIAL_DEVICE_NAME);
        serial_tx_line(hello);
        engine_resync(&s_serial_sink);

    } else if (strcmp(cmd, SERIAL_CMD_START) == 0) {
        /* Parse optional params and electrode */
        dpv_params_t params = DPV_PARAMS_DEFAULT;

        cJSON *elec_j = cJSON_GetObjectItem(j, "electrode");
        uint8_t electrode = (elec_j && cJSON_IsNumber(elec_j))
                            ? (uint8_t)elec_j->valuedouble : 1u;
        params.electrode = (electrode_t)electrode;

        cJSON *p = cJSON_GetObjectItem(j, "params");
        if (p) {
#define GET_NUM(field, key)                                          \
    do {                                                             \
        cJSON *_x = cJSON_GetObjectItem(p, key);                     \
        if (_x && cJSON_IsNumber(_x))                                \
            params.field = (typeof(params.field))_x->valuedouble;   \
    } while (0)

            GET_NUM(e_begin_mV,         SERIAL_PARAM_E_BEGIN_MV);
            GET_NUM(e_end_mV,           SERIAL_PARAM_E_END_MV);
            GET_NUM(e_step_mV,          SERIAL_PARAM_E_STEP_MV);
            GET_NUM(e_pulse_mV,         SERIAL_PARAM_E_PULSE_MV);
            GET_NUM(t_pulse_ms,         SERIAL_PARAM_T_PULSE_MS);
            GET_NUM(t_period_ms,        SERIAL_PARAM_T_PERIOD_MS);
            GET_NUM(t_equilibration_ms, SERIAL_PARAM_T_EQUILIBRATION_MS);
            GET_NUM(cycles,             SERIAL_PARAM_CYCLES);
            GET_NUM(n_avg,              SERIAL_PARAM_N_AVG);
#undef GET_NUM
        }

        esp_err_t err = engine_start(electrode, &params);
        if (err != ESP_OK) {
            cJSON *err_j = cJSON_CreateObject();
            if (err_j) {
                cJSON_AddStringToObject(err_j, "t",   SERIAL_T_ERROR);
                cJSON_AddStringToObject(err_j, "msg", esp_err_to_name(err));
                char *s = cJSON_PrintUnformatted(err_j);
                cJSON_Delete(err_j);
                if (s) { serial_tx_line(s); free(s); }
            }
        }

    } else {
        ESP_LOGW(TAG, "RX: unknown cmd '%s'", cmd);
    }

    cJSON_Delete(j);
}

/* --------------------------------------------------------------------------
 * RX task — line accumulator → command dispatcher
 * -------------------------------------------------------------------------- */

static void serial_rx_task(void *arg)
{
    (void)arg;

    char     line[SERIAL_CMD_LINE_MAX];
    int      line_len = 0;
    uint8_t  chunk[SERIAL_CHUNK_BYTES];

    for (;;) {
        int n = uart_read_bytes(SERIAL_UART_NUM, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(50));
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    /* Only process lines that look like JSON objects */
                    if (line[0] == '{') {
                        serial_process_cmd(line, line_len);
                    }
                    /* Silently drop non-JSON lines (stray log output, etc.) */
                    line_len = 0;
                }
                /* else: skip empty lines */
            } else if (line_len < (int)SERIAL_CMD_LINE_MAX - 1) {
                line[line_len++] = c;
            } else {
                /* Line overflow — discard partial line, reset */
                ESP_LOGW(TAG, "RX: line overflow, discarding");
                line_len = 0;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

esp_err_t serial_comms_start(void)
{
    /* ---- UART0 configuration ---- */
    uart_config_t cfg = {
        .baud_rate  = CONFIG_UBIOPOT_SERIAL_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(SERIAL_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(ret));
        return ret;
    }

    /* GPIO1 = U0TXD, GPIO3 = U0RXD — UART_PIN_NO_CHANGE keeps them as-is */
    ret = uart_set_pin(SERIAL_UART_NUM,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install driver — if already installed (e.g. console), reinstall is safe */
    if (!uart_is_driver_installed(SERIAL_UART_NUM)) {
        ret = uart_driver_install(SERIAL_UART_NUM,
                                  SERIAL_RX_BUF_BYTES,
                                  SERIAL_TX_BUF_BYTES,
                                  0, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* Console already installed UART0; update baud rate to our value */
        uart_set_baudrate(SERIAL_UART_NUM, CONFIG_UBIOPOT_SERIAL_BAUD);
        ESP_LOGI(TAG, "UART0 driver already installed; baud updated to %d",
                 CONFIG_UBIOPOT_SERIAL_BAUD);
    }

    /* ---- Log routing (Option A): suppress INFO/DEBUG on UART0 ----
     * ERROR and WARN still pass — host must skip lines not starting with '{'. */
    esp_log_level_set("*", ESP_LOG_WARN);
    ESP_LOGW(TAG, "Log level set to WARN for NDJSON stream on UART0");

    /* ---- TX mutex ---- */
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) {
        ESP_LOGE(TAG, "Failed to create TX mutex");
        return ESP_ERR_NO_MEM;
    }

    /* ---- Register engine sink ---- */
    s_serial_sink.on_point  = serial_on_point;
    s_serial_sink.on_event  = serial_on_event;
    s_serial_sink.on_resync = serial_on_resync;
    s_serial_sink.ctx       = NULL;

    ret = engine_register_sink(&s_serial_sink);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "engine_register_sink: %s", esp_err_to_name(ret));
        /* Non-fatal: serial will still accept commands; just won't receive scan data */
    }

    /* ---- Emit hello handshake ---- */
    char hello[128];
    snprintf(hello, sizeof(hello),
        "{\"t\":\"%s\",\"%s\":\"%s\",\"%s\":%d,\"%s\":\"%s\"}",
        SERIAL_T_HELLO,
        SERIAL_HELLO_FW,     SERIAL_FW_VERSION_STR,
        SERIAL_HELLO_PROTO,  SERIAL_PROTOCOL_VERSION,
        SERIAL_HELLO_DEVICE, SERIAL_DEVICE_NAME);
    serial_tx_line(hello);

    /* Resync any in-progress scan (e.g. if serial_comms_start called after scan begins) */
    engine_resync(&s_serial_sink);

    /* ---- Spawn RX task on Core 0 (same as Dispatcher, lower priority) ---- */
    BaseType_t ok = xTaskCreatePinnedToCore(
        serial_rx_task,
        "serial_rx",
        RX_TASK_STACK,
        NULL,
        RX_TASK_PRIO,
        NULL,
        0 /* Core 0 */
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_rx task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "serial_comms started: UART0 %d baud, proto v%d",
             CONFIG_UBIOPOT_SERIAL_BAUD, SERIAL_PROTOCOL_VERSION);
    return ESP_OK;
}
