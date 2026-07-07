/**
 * @file net_comms.c
 * @brief P5 — WiFi APSTA, captive portal, mDNS, HTTP server, WebSocket engine sink.
 *
 * Architecture recap (from plan):
 *   - SoftAP always on (SSID Aqua-HMET-<MAC4>, WPA2, GW 192.168.4.1).
 *   - STA optional (NVS creds; not provisioned yet in P5 — wired in P8/Kconfig).
 *   - UDP:53 captive DNS task answers every A-query → 192.168.4.1.
 *   - mDNS: hostname aquahmet → aquahmet.local, _http._tcp.
 *   - esp_http_server on Core 0.
 *   - LittleFS mounted at VFS path "/www" (partition label "littlefs").
 *   - WS /ws: binary DataPoint frames (16 B) from Dispatcher via httpd_queue_work.
 *   - WS inbound control: {"cmd":"start"/"abort"/"zero"/"state"/"hello"}.
 *   - HTTP REST API: /api/state /api/scan /api/abort /api/scan.csv /api/reference.csv /api/wifi.
 *   - net_comms registers as engine sink; on_point → WS broadcast; on_event → JSON WS.
 *   - After WiFi up: calls ui_tft_set_wifi_info(ssid, ip, url).
 *
 * Thread-safety rule: ALL httpd/WS sends from non-httpd tasks go via httpd_queue_work.
 */

#include "net_comms.h"
#include "net_comms_protocol.h"   /* pure wire-format defs, IDF-free */
#include "acq_engine.h"
#include "echem_core/scan_state.h"
#include "echem_core/dpv.h"
#include "ui_tft.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "mdns.h"
#include "cJSON.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "net_comms";

/* --------------------------------------------------------------------------
 * Configuration constants
 * -------------------------------------------------------------------------- */

#define LITTLEFS_BASE_PATH     "/www"
#define AP_GATEWAY_IP          "192.168.4.1"
#define WS_PATH                "/ws"
#define MAX_WS_CLIENTS         4
#define WS_WORK_QUEUE_DEPTH    8
#define DNS_PORT               53
#define DNS_TASK_STACK         3072
#define DNS_MAX_PKT            512

/* Default STA credentials — override at runtime via POST /api/wifi (P5)
 * or via Kconfig (P8 NVS).  Empty strings = STA disabled at first boot. */
#ifndef CONFIG_UBIOPOT_WIFI_STA_SSID
#define CONFIG_UBIOPOT_WIFI_STA_SSID  ""
#endif
#ifndef CONFIG_UBIOPOT_WIFI_STA_PASS
#define CONFIG_UBIOPOT_WIFI_STA_PASS  ""
#endif

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */

static httpd_handle_t s_server    = NULL;
static SemaphoreHandle_t s_ws_mutex = NULL; /* protects s_ws_fds[] */
static int  s_ws_fds[MAX_WS_CLIENTS];
static int  s_ws_fd_count         = 0;
static char s_ap_ssid[33]         = {0};
static char s_sta_ip[16]          = {0};    /* STA IP if connected */

/* Local copy of scan points for CSV export (updated by sink callbacks) */
#define NET_SCAN_BUF_MAX   ENGINE_SCAN_BUF_MAX
static DataPoint  s_scan_buf[NET_SCAN_BUF_MAX];
static uint16_t   s_scan_count    = 0;
static SemaphoreHandle_t s_scan_mutex = NULL;

/* Sink handle (kept so we can pass ctx=&s_net_sink to engine) */
static engine_sink_t s_net_sink;

/* Binary frame type alias — uses definitions from net_comms_protocol.h */
typedef net_ws_dp_frame_t ws_dp_frame_t;
#define WS_FRAME_TYPE_DATAPOINT  NET_WS_FRAME_TYPE_DATAPOINT

/* --------------------------------------------------------------------------
 * WS async-send work struct (heap-allocated; freed by work function)
 * -------------------------------------------------------------------------- */

typedef struct {
    httpd_handle_t server;
    int            fd;
    uint8_t       *payload;
    size_t         len;
    bool           is_text;    /* false = HTTPD_WS_TYPE_BINARY */
} ws_send_work_t;

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static void ws_fd_add(int fd);
static void ws_fd_remove(int fd);
static void ws_send_work_fn(void *arg);
static esp_err_t ws_send_to_fd(int fd, const uint8_t *data, size_t len, bool is_text);
static void ws_broadcast(const uint8_t *data, size_t len, bool is_text);
static esp_err_t start_http_server(void);

/* --------------------------------------------------------------------------
 * WS client-fd management
 * -------------------------------------------------------------------------- */

static void ws_fd_add(int fd)
{
    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (int i = 0; i < s_ws_fd_count; i++) {
        if (s_ws_fds[i] == fd) {
            xSemaphoreGive(s_ws_mutex);
            return; /* already tracked */
        }
    }
    if (s_ws_fd_count < MAX_WS_CLIENTS) {
        s_ws_fds[s_ws_fd_count++] = fd;
        ESP_LOGI(TAG, "WS client added fd=%d (total=%d)", fd, s_ws_fd_count);
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_fd_remove(int fd)
{
    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (int i = 0; i < s_ws_fd_count; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = s_ws_fds[--s_ws_fd_count];
            ESP_LOGI(TAG, "WS client removed fd=%d (total=%d)", fd, s_ws_fd_count);
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

/* --------------------------------------------------------------------------
 * WS async send helpers (called from Dispatcher task → httpd_queue_work)
 * -------------------------------------------------------------------------- */

static void ws_send_work_fn(void *arg)
{
    ws_send_work_t *w = (ws_send_work_t *)arg;
    if (!w) return;

    /* Verify the fd is still a WS connection before sending */
    if (httpd_ws_get_fd_info(w->server, w->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame = {
            .type    = w->is_text ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY,
            .payload = w->payload,
            .len     = w->len,
            .final   = true,
        };
        esp_err_t err = httpd_ws_send_frame_async(w->server, w->fd, &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WS send err fd=%d: %s", w->fd, esp_err_to_name(err));
            ws_fd_remove(w->fd);
        }
    } else {
        ws_fd_remove(w->fd);
    }

    free(w->payload);
    free(w);
}

static esp_err_t ws_send_to_fd(int fd, const uint8_t *data, size_t len, bool is_text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_send_work_t *w = malloc(sizeof(ws_send_work_t));
    if (!w) return ESP_ERR_NO_MEM;

    w->payload = malloc(len);
    if (!w->payload) { free(w); return ESP_ERR_NO_MEM; }

    memcpy(w->payload, data, len);
    w->server  = s_server;
    w->fd      = fd;
    w->len     = len;
    w->is_text = is_text;

    esp_err_t err = httpd_queue_work(s_server, ws_send_work_fn, w);
    if (err != ESP_OK) {
        free(w->payload);
        free(w);
    }
    return err;
}

/* Broadcast to all tracked WS clients — called from Dispatcher (Core 0) */
static void ws_broadcast(const uint8_t *data, size_t len, bool is_text)
{
    if (!s_server) return;
    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    int fds[MAX_WS_CLIENTS];
    int n = s_ws_fd_count;
    memcpy(fds, s_ws_fds, n * sizeof(int));
    xSemaphoreGive(s_ws_mutex);

    for (int i = 0; i < n; i++) {
        ws_send_to_fd(fds[i], data, len, is_text);
    }
}

/* --------------------------------------------------------------------------
 * Engine sink callbacks
 * -------------------------------------------------------------------------- */

static void net_on_point(const DataPoint *pt, void *ctx)
{
    (void)ctx;

    /* 1. Append to local scan buffer (for CSV export) */
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_scan_count < NET_SCAN_BUF_MAX) {
            s_scan_buf[s_scan_count++] = *pt;
        }
        xSemaphoreGive(s_scan_mutex);
    }

    /* 2. Broadcast binary DataPoint frame over WS */
    ws_dp_frame_t frame = {
        .frame_type = WS_FRAME_TYPE_DATAPOINT,
        .electrode  = pt->electrode,
        .idx        = pt->idx,
        .E_mV       = pt->E_mV,
        .I_uA       = pt->I_uA,
        .RE_mV      = pt->RE_mV,
    };
    ws_broadcast((uint8_t *)&frame, sizeof(frame), false /* binary */);
}

static void net_on_event(scan_event_t evt, const char *info, void *ctx)
{
    (void)ctx;

    /* Reset local scan buffer on new scan start */
    if (evt == SCAN_EVT_START) {
        if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_scan_count = 0;
            xSemaphoreGive(s_scan_mutex);
        }
    }

    /* Build JSON event */
    const char *name = NULL;
    switch (evt) {
        case SCAN_EVT_START:        name = "scan_started";  break;
        case SCAN_EVT_EQUILIB_DONE: name = "equilibrating"; break;
        case SCAN_EVT_SCAN_DONE:    name = "scan_complete"; break;
        case SCAN_EVT_ABORTED:      name = "scan_aborted";  break;
        case SCAN_EVT_ERROR:        name = "scan_error";    break;
        case SCAN_EVT_RESET:        name = "scan_reset";    break;
        default:                    name = "unknown";       break;
    }

    cJSON *j = cJSON_CreateObject();
    if (!j) return;
    cJSON_AddStringToObject(j, "t", "event");
    cJSON_AddStringToObject(j, "name", name);
    if (info && *info) {
        cJSON_AddStringToObject(j, "msg", info);
    }

    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return;

    ws_broadcast((uint8_t *)s, strlen(s), true /* text */);
    free(s);
}

/* Resync targeted to a single fd — ctx carries the fd as (void*)(intptr_t)fd */
static void net_on_resync(const DataPoint *buf, uint16_t count,
                          scan_state_t state, void *ctx)
{
    /* ctx encodes the target fd: non-NULL means unicast; NULL means broadcast.
     * The broadcast path is only used if engine_resync() is ever called without a
     * specific client context (not done after this fix; retained for safety). */
    int target_fd = ctx ? (int)(intptr_t)ctx : -1;

    /* Update local scan buffer (always — keeps it current regardless of send path) */
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint16_t n = count < NET_SCAN_BUF_MAX ? count : NET_SCAN_BUF_MAX;
        if (buf && n > 0) memcpy(s_scan_buf, buf, n * sizeof(DataPoint));
        s_scan_count = n;
        xSemaphoreGive(s_scan_mutex);
    }

    /* Send each point as a binary frame to the target fd only */
    for (uint16_t i = 0; i < count && i < NET_SCAN_BUF_MAX; i++) {
        ws_dp_frame_t frame = {
            .frame_type = WS_FRAME_TYPE_DATAPOINT,
            .electrode  = buf[i].electrode,
            .idx        = buf[i].idx,
            .E_mV       = buf[i].E_mV,
            .I_uA       = buf[i].I_uA,
            .RE_mV      = buf[i].RE_mV,
        };
        if (target_fd >= 0) {
            ws_send_to_fd(target_fd, (uint8_t *)&frame, sizeof(frame), false);
        } else {
            ws_broadcast((uint8_t *)&frame, sizeof(frame), false);
        }
    }

    /* Send resync_complete JSON */
    cJSON *j = cJSON_CreateObject();
    if (j) {
        cJSON_AddStringToObject(j, "t", "resync_complete");
        cJSON_AddNumberToObject(j, "pts", count);
        cJSON_AddNumberToObject(j, "state", (double)state);
        char *s = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (s) {
            if (target_fd >= 0) {
                ws_send_to_fd(target_fd, (uint8_t *)s, strlen(s), true);
            } else {
                ws_broadcast((uint8_t *)s, strlen(s), true);
            }
            free(s);
        }
    }
}

/* --------------------------------------------------------------------------
 * WebSocket URI handler  (/ws)
 * -------------------------------------------------------------------------- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        /* WS upgrade handshake — register client */
        ws_fd_add(fd);
        ESP_LOGI(TAG, "WS handshake fd=%d", fd);

        /* Send handshake hello + resync any in-progress scan */
        cJSON *hello = cJSON_CreateObject();
        if (hello) {
            cJSON_AddStringToObject(hello, "t", "hello");
            cJSON_AddStringToObject(hello, "fw", "1.0.0");
            cJSON_AddNumberToObject(hello, "proto", 1);
            char *s = cJSON_PrintUnformatted(hello);
            cJSON_Delete(hello);
            if (s) {
                httpd_ws_frame_t f = {
                    .type    = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)s,
                    .len     = strlen(s),
                    .final   = true,
                };
                httpd_ws_send_frame(req, &f);
                free(s);
            }
        }

        /* Trigger resync targeted to this fd only — not a broadcast.
         * We create a temporary sink whose ctx carries the fd so net_on_resync
         * sends only to this client, not to all existing connections. */
        engine_sink_t fd_sink = s_net_sink;
        fd_sink.ctx = (void *)(intptr_t)fd;
        engine_resync(&fd_sink);
        return ESP_OK;
    }

    /* Receive an incoming WS frame */
    httpd_ws_frame_t rx = {0};
    uint8_t rx_buf[256] = {0};
    rx.payload = rx_buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &rx, sizeof(rx_buf) - 1);
    if (ret != ESP_OK) return ret;

    if (rx.type == HTTPD_WS_TYPE_CLOSE) {
        ws_fd_remove(fd);
        return ESP_OK;
    }

    if (rx.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    /* Null-terminate and parse JSON command */
    rx_buf[rx.len] = '\0';
    cJSON *cmd_j = cJSON_ParseWithLength((char *)rx_buf, rx.len);
    if (!cmd_j) {
        ESP_LOGW(TAG, "WS rx: bad JSON: %s", rx_buf);
        return ESP_OK;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(cmd_j, "cmd");
    const char *cmd = cmd_item ? cJSON_GetStringValue(cmd_item) : NULL;
    if (!cmd) { cJSON_Delete(cmd_j); return ESP_OK; }

    if (strcmp(cmd, "abort") == 0) {
        engine_abort();

    } else if (strcmp(cmd, "zero") == 0) {
        engine_zero();

    } else if (strcmp(cmd, "state") == 0) {
        cJSON *resp = cJSON_CreateObject();
        if (resp) {
            cJSON_AddStringToObject(resp, "t", "state");
            cJSON_AddNumberToObject(resp, "state", (double)engine_get_state());
            char *s = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            if (s) {
                httpd_ws_frame_t f = {
                    .type    = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)s,
                    .len     = strlen(s),
                    .final   = true,
                };
                httpd_ws_send_frame(req, &f);
                free(s);
            }
        }

    } else if (strcmp(cmd, "hello") == 0) {
        /* Client re-hello: resync targeted to this fd only */
        engine_sink_t fd_sink = s_net_sink;
        fd_sink.ctx = (void *)(intptr_t)fd;
        engine_resync(&fd_sink);

    } else if (strcmp(cmd, "start") == 0) {
        /* Parse params */
        cJSON *p_j     = cJSON_GetObjectItem(cmd_j, "params");
        cJSON *elec_j  = cJSON_GetObjectItem(cmd_j, "electrode");
        uint8_t electrode = elec_j ? (uint8_t)elec_j->valuedouble : 1;

        dpv_params_t params = DPV_PARAMS_DEFAULT;
        params.electrode = (electrode_t)electrode;

        if (p_j) {
#define GET_NUM(field, key) { cJSON *x = cJSON_GetObjectItem(p_j, key); \
    if (x && cJSON_IsNumber(x)) params.field = (typeof(params.field))x->valuedouble; }
            GET_NUM(e_begin_mV,        "e_begin_mV")
            GET_NUM(e_end_mV,          "e_end_mV")
            GET_NUM(e_step_mV,         "e_step_mV")
            GET_NUM(e_pulse_mV,        "e_pulse_mV")
            GET_NUM(t_pulse_ms,        "t_pulse_ms")
            GET_NUM(t_period_ms,       "t_period_ms")
            GET_NUM(t_equilibration_ms,"t_equilibration_ms")
            GET_NUM(cycles,            "cycles")
            GET_NUM(n_avg,             "n_avg")
#undef GET_NUM
        }

        esp_err_t start_err = engine_start(electrode, &params);
        if (start_err != ESP_OK) {
            cJSON *err_j = cJSON_CreateObject();
            if (err_j) {
                cJSON_AddStringToObject(err_j, "t", "error");
                cJSON_AddStringToObject(err_j, "msg", esp_err_to_name(start_err));
                char *s = cJSON_PrintUnformatted(err_j);
                cJSON_Delete(err_j);
                if (s) {
                    httpd_ws_frame_t f = {
                        .type    = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t *)s,
                        .len     = strlen(s),
                        .final   = true,
                    };
                    httpd_ws_send_frame(req, &f);
                    free(s);
                }
            }
        }
    }

    cJSON_Delete(cmd_j);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * HTTP REST API handlers
 * -------------------------------------------------------------------------- */

static esp_err_t api_state_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    cJSON *j = cJSON_CreateObject();
    if (!j) { httpd_resp_send_500(req); return ESP_OK; }

    const char *state_names[] = {
        "IDLE", "EQUILIBRATING", "RUNNING", "COMPLETE", "ABORTING", "ERROR"
    };
    scan_state_t st = engine_get_state();
    int st_idx = (int)st;
    if (st_idx < 0 || st_idx > 5) st_idx = 5;

    cJSON_AddStringToObject(j, "state", state_names[st_idx]);
    cJSON_AddNumberToObject(j, "state_code", st_idx);

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        cJSON_AddNumberToObject(j, "pts", s_scan_count);
        xSemaphoreGive(s_scan_mutex);
    }

    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) { httpd_resp_send_500(req); return ESP_OK; }
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    /* Read JSON body */
    char body[512] = {0};
    int len = MIN((int)req->content_len, (int)sizeof(body) - 1);
    if (len > 0 && httpd_req_recv(req, body, len) <= 0) {
        httpd_resp_send_408(req);
        return ESP_OK;
    }
    body[len] = '\0';

    dpv_params_t params = DPV_PARAMS_DEFAULT;
    uint8_t electrode = 1;

    cJSON *j = cJSON_ParseWithLength(body, strlen(body));
    if (j) {
        cJSON *elec_j = cJSON_GetObjectItem(j, "electrode");
        if (elec_j && cJSON_IsNumber(elec_j)) electrode = (uint8_t)elec_j->valuedouble;
        params.electrode = (electrode_t)electrode;
        cJSON *p = cJSON_GetObjectItem(j, "params");
        if (p) {
#define GN(field, key) { cJSON *x = cJSON_GetObjectItem(p, key); \
    if (x && cJSON_IsNumber(x)) params.field = (typeof(params.field))x->valuedouble; }
            GN(e_begin_mV,        "e_begin_mV")
            GN(e_end_mV,          "e_end_mV")
            GN(e_step_mV,         "e_step_mV")
            GN(e_pulse_mV,        "e_pulse_mV")
            GN(t_pulse_ms,        "t_pulse_ms")
            GN(t_period_ms,       "t_period_ms")
            GN(t_equilibration_ms,"t_equilibration_ms")
            GN(cycles,            "cycles")
            GN(n_avg,             "n_avg")
#undef GN
        }
        cJSON_Delete(j);
    }

    esp_err_t err = engine_start(electrode, &params);

    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    char resp[64];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"ok\":true}");
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(err));
    }
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_abort_handler(httpd_req_t *req)
{
    engine_abort();
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_csv_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, NET_CSV_MIME);
    httpd_resp_set_hdr(req, "Content-Disposition", NET_CSV_DISPOSITION);

    /* Write header */
    httpd_resp_send_chunk(req, NET_CSV_HEADER, strlen(NET_CSV_HEADER));

    /* Write data */
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char line[80];
        for (uint16_t i = 0; i < s_scan_count; i++) {
            const DataPoint *dp = &s_scan_buf[i];
            int n = snprintf(line, sizeof(line), "%u,%u,%.4f,%.4f,%.4f\r\n",
                             dp->electrode, dp->idx,
                             (double)dp->E_mV, (double)dp->I_uA, (double)dp->RE_mV);
            if (n > 0) httpd_resp_send_chunk(req, line, n);
        }
        xSemaphoreGive(s_scan_mutex);
    }

    /* Terminate chunked transfer */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_reference_csv_handler(httpd_req_t *req)
{
    /* Store uploaded reference CSV to LittleFS so the SPA can overlay it */
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    const size_t MAX_REF = 32768;
    if (req->content_len > MAX_REF) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Max 32 KB");
        return ESP_OK;
    }

    FILE *f = fopen(LITTLEFS_BASE_PATH "/reference.csv", "w");
    if (!f) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    char chunk[256];
    int remaining = (int)req->content_len;
    bool ok = true;
    while (remaining > 0 && ok) {
        int to_read = MIN(remaining, (int)sizeof(chunk));
        int got = httpd_req_recv(req, chunk, to_read);
        if (got <= 0) { ok = false; break; }
        if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
            ESP_LOGE(TAG, "reference CSV fwrite failed (LittleFS full?)");
            ok = false;
            break;
        }
        remaining -= got;
    }
    fclose(f);

    if (!ok) {
        /* Remove the partial file so a subsequent upload isn't corrupted */
        remove(LITTLEFS_BASE_PATH "/reference.csv");
        httpd_resp_send_408(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_wifi_handler(httpd_req_t *req)
{
    /* POST /api/wifi — provision STA credentials.
     * Body: {"ssid":"...","password":"..."}
     * Stores to NVS (P8 wires NVS; for now just attempts to connect). */
    char body[256] = {0};
    int len = MIN((int)req->content_len, (int)sizeof(body) - 1);
    if (len > 0 && httpd_req_recv(req, body, len) <= 0) {
        httpd_resp_send_408(req);
        return ESP_OK;
    }
    body[len] = '\0';

    cJSON *j = cJSON_ParseWithLength(body, strlen(body));
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_OK;
    }

    cJSON *ssid_j = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(j, "password");
    const char *ssid = ssid_j ? cJSON_GetStringValue(ssid_j) : NULL;
    const char *pass = pass_j ? cJSON_GetStringValue(pass_j) : "";

    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 31) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID");
        return ESP_OK;
    }

    /* Attempt STA reconnect with new credentials */
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    cJSON_Delete(j);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Connecting...\"}");
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Static file server (LittleFS assets for P6 SPA)
 *
 * ends_with(), net_content_type(), net_path_is_traversal() all come from
 * net_comms_protocol.h (inline, pure C, host-testable).
 * -------------------------------------------------------------------------- */

/* Try path as-is, then path.gz, then path/index.html, then /index.html */
static esp_err_t serve_file(httpd_req_t *req, const char *filepath)
{
    /* Try gzip version first */
    char gz_path[180];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);

    struct stat st;
    const char *actual_path = filepath;
    bool gzip = false;

    if (stat(gz_path, &st) == 0) {
        actual_path = gz_path;
        gzip = true;
    } else if (stat(filepath, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(actual_path, "rb");
    if (!f) return ESP_FAIL;

    httpd_resp_set_type(req, net_content_type(filepath));
    if (gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); /* end chunked */
    return ESP_OK;
}

static esp_err_t file_server_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* Strip query string */
    char uri_clean[160];
    strlcpy(uri_clean, uri, sizeof(uri_clean));
    char *qs = strchr(uri_clean, '?');
    if (qs) *qs = '\0';

    /* Reject path traversal */
    if (net_path_is_traversal(uri_clean)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    /* Map URI to filesystem path — base("/www") + uri, needs 4+160=164 bytes */
    char filepath[176];
    snprintf(filepath, sizeof(filepath), "%s%s", LITTLEFS_BASE_PATH, uri_clean);

    /* Handle trailing slash or bare / → index.html */
    size_t flen = strlen(filepath);
    if (filepath[flen - 1] == '/') {
        strlcat(filepath, "index.html", sizeof(filepath));
    }

    esp_err_t err = serve_file(req, filepath);
    if (err == ESP_ERR_NOT_FOUND) {
        /* Try appending /index.html for SPA client-side routing */
        snprintf(filepath, sizeof(filepath), "%s/index.html", LITTLEFS_BASE_PATH);
        err = serve_file(req, filepath);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Captive portal probe handlers
 * -------------------------------------------------------------------------- */

/* Android connectivity check */
static esp_err_t captive_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Apple connectivity check */
static esp_err_t captive_apple_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return ESP_OK;
}

/* Windows connectivity check */
static esp_err_t captive_win_ncsi_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

static esp_err_t captive_win_connect_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "Microsoft Connect Test");
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * HTTP server startup
 * -------------------------------------------------------------------------- */

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id          = 0;                /* Core 0 (same as WiFi/LVGL) */
    cfg.task_priority    = 5;
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = MAX_WS_CLIENTS + 5; /* WS + HTTP clients */
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* --- WebSocket /ws --- */
    static const httpd_uri_t ws_uri = {
        .uri      = WS_PATH,
        .method   = HTTP_GET,
        .handler  = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* --- REST API --- */
    static const httpd_uri_t state_uri = {
        .uri = "/api/state", .method = HTTP_GET, .handler = api_state_handler };
    static const httpd_uri_t scan_uri = {
        .uri = "/api/scan",  .method = HTTP_POST, .handler = api_scan_handler };
    static const httpd_uri_t abort_uri = {
        .uri = "/api/abort", .method = HTTP_POST, .handler = api_abort_handler };
    static const httpd_uri_t csv_uri = {
        .uri = "/api/scan.csv", .method = HTTP_GET, .handler = api_csv_handler };
    static const httpd_uri_t ref_uri = {
        .uri = "/api/reference.csv", .method = HTTP_POST, .handler = api_reference_csv_handler };
    static const httpd_uri_t wifi_uri = {
        .uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_handler };

    httpd_register_uri_handler(s_server, &state_uri);
    httpd_register_uri_handler(s_server, &scan_uri);
    httpd_register_uri_handler(s_server, &abort_uri);
    httpd_register_uri_handler(s_server, &csv_uri);
    httpd_register_uri_handler(s_server, &ref_uri);
    httpd_register_uri_handler(s_server, &wifi_uri);

    /* --- Captive portal probes --- */
    static const httpd_uri_t gen204 = {
        .uri = "/generate_204", .method = HTTP_GET, .handler = captive_204_handler };
    static const httpd_uri_t apple = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_apple_handler };
    static const httpd_uri_t ncsi = {
        .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_win_ncsi_handler };
    static const httpd_uri_t winconn = {
        .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_win_connect_handler };

    httpd_register_uri_handler(s_server, &gen204);
    httpd_register_uri_handler(s_server, &apple);
    httpd_register_uri_handler(s_server, &ncsi);
    httpd_register_uri_handler(s_server, &winconn);

    /* --- Wildcard file server (must be last) --- */
    static const httpd_uri_t files_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = file_server_handler };
    httpd_register_uri_handler(s_server, &files_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Captive portal DNS responder
 *
 * Listens on UDP:53 and responds to ALL A-queries with 192.168.4.1.
 * This causes phone OS captive-portal detectors to pop the dashboard.
 * -------------------------------------------------------------------------- */

static void dns_responder_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket open failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Set receive timeout so we don't block forever */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[DNS_MAX_PKT];
    uint8_t resp[DNS_MAX_PKT];
    struct sockaddr_in client;
    socklen_t clen;

    /* Parse AP IP: 192.168.4.1 */
    uint32_t ap_ip_net;
    inet_pton(AF_INET, AP_GATEWAY_IP, &ap_ip_net);

    ESP_LOGI(TAG, "Captive DNS listening on UDP:53");

    while (true) {
        clen = sizeof(client);
        int n = recvfrom(sock, pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue; /* too short for DNS header */

        /* DNS header: ID(2) Flags(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2) */
        uint16_t qdcount = (uint16_t)(pkt[4] << 8 | pkt[5]);
        if (qdcount == 0) continue;

        /* Build response: copy header, set QR=1, AA=1, ANCOUNT=1 */
        memcpy(resp, pkt, n);
        resp[2] = 0x81; /* QR=1 OPCODE=0 AA=1 */
        resp[3] = 0x80; /* RA=1 */
        resp[6] = 0;    /* ANCOUNT high */
        resp[7] = 1;    /* ANCOUNT = 1 */
        resp[8] = 0;    /* NSCOUNT */
        resp[9] = 0;
        resp[10] = 0;   /* ARCOUNT */
        resp[11] = 0;

        /* Append Answer RR after the question section */
        /* Pointer to question name: 0xC00C (byte 12 = start of QNAME) */
        int qpos = 12;
        /* Skip QNAME (null-terminated labels) */
        while (qpos < n && pkt[qpos] != 0) {
            if ((pkt[qpos] & 0xC0) == 0xC0) { qpos += 2; goto skip; }
            qpos += pkt[qpos] + 1;
        }
        qpos++; /* past null byte */
        skip:
        qpos += 4; /* QTYPE + QCLASS */

        /* Answer section */
        if (qpos + 16 < DNS_MAX_PKT) {
            resp[qpos++] = 0xC0; /* NAME pointer */
            resp[qpos++] = 0x0C;
            resp[qpos++] = 0;    /* TYPE = A */
            resp[qpos++] = 1;
            resp[qpos++] = 0;    /* CLASS = IN */
            resp[qpos++] = 1;
            resp[qpos++] = 0;    /* TTL = 1 s */
            resp[qpos++] = 0;
            resp[qpos++] = 0;
            resp[qpos++] = 1;
            resp[qpos++] = 0;    /* RDLENGTH = 4 */
            resp[qpos++] = 4;
            memcpy(&resp[qpos], &ap_ip_net, 4);
            qpos += 4;

            sendto(sock, resp, qpos, 0, (struct sockaddr *)&client, clen);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * mDNS
 * -------------------------------------------------------------------------- */

static void mdns_setup(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_UBIOPOT_MDNS_HOSTNAME);
    mdns_instance_name_set("Aqua-HMET Potentiostat");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", CONFIG_UBIOPOT_MDNS_HOSTNAME);
}

/* --------------------------------------------------------------------------
 * LittleFS
 * -------------------------------------------------------------------------- */

static esp_err_t littlefs_mount(void)
{
    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = LITTLEFS_BASE_PATH,
        .partition_label        = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "LittleFS: %zu/%zu bytes used", used, total);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * WiFi event handler
 * -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started: SSID=%s", s_ap_ssid);
        /* Update TFT with AP info immediately */
        char url[64];
        snprintf(url, sizeof(url), "http://%s", AP_GATEWAY_IP);
        ui_tft_set_wifi_info(s_ap_ssid, AP_GATEWAY_IP, url);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&ev->ip_info.ip, s_sta_ip, sizeof(s_sta_ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
        /* Prefer STA IP for the URL shown on TFT */
        char url[64];
        snprintf(url, sizeof(url), "http://%s.local", CONFIG_UBIOPOT_MDNS_HOSTNAME);
        ui_tft_set_wifi_info(s_ap_ssid, s_sta_ip, url);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected — retrying");
        esp_wifi_connect();
        s_sta_ip[0] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * WiFi init
 * -------------------------------------------------------------------------- */

static esp_err_t wifi_init(void)
{
    esp_err_t err;

    /* Init netif + event loop */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    /* Init WiFi driver (must precede esp_wifi_get_mac) */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) return err;

    /* Derive SoftAP SSID from MAC (requires wifi_init done) */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             CONFIG_UBIOPOT_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);

    /* Register event handlers */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    /* Configure SoftAP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = 0,
            .channel        = 1,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = CONFIG_UBIOPOT_WIFI_AP_MAX_CONN,
            .beacon_interval = 100,
        }
    };
    strlcpy((char *)ap_cfg.ap.ssid,     s_ap_ssid,
            sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, CONFIG_UBIOPOT_WIFI_AP_PASSWORD,
            sizeof(ap_cfg.ap.password));
    /* Open auth if password is empty */
    if (strlen(CONFIG_UBIOPOT_WIFI_AP_PASSWORD) < 8) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    /* Configure STA if compile-time creds provided (P8 will load from NVS) */
    if (strlen(CONFIG_UBIOPOT_WIFI_STA_SSID) > 0) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid,     CONFIG_UBIOPOT_WIFI_STA_SSID,
                sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, CONFIG_UBIOPOT_WIFI_STA_PASS,
                sizeof(sta_cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    /* Attempt STA connection if creds present */
    if (strlen(CONFIG_UBIOPOT_WIFI_STA_SSID) > 0) {
        esp_wifi_connect();
    }

    ESP_LOGI(TAG, "WiFi started — AP SSID: %s", s_ap_ssid);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * net_comms_start — public entry point
 * -------------------------------------------------------------------------- */

esp_err_t net_comms_start(void)
{
    esp_err_t err;

    /* 0. Create synchronisation primitives */
    s_ws_mutex   = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex || !s_scan_mutex) return ESP_ERR_NO_MEM;

    memset(s_ws_fds, -1, sizeof(s_ws_fds));

    /* 1. Mount LittleFS web assets */
    err = littlefs_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — web assets unavailable");
        /* Non-fatal: HTTP server will return 404 for static files */
    }

    /* 2. WiFi APSTA */
    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Captive portal DNS task (Core 0) */
    xTaskCreatePinnedToCore(dns_responder_task, "captive_dns",
                            DNS_TASK_STACK, NULL, 5, NULL, 0);

    /* 4. mDNS */
    mdns_setup();

    /* 5. HTTP server + WebSocket */
    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 6. Register as engine sink */
    s_net_sink.on_point  = net_on_point;
    s_net_sink.on_event  = net_on_event;
    s_net_sink.on_resync = net_on_resync;
    s_net_sink.ctx       = NULL;
    err = engine_register_sink(&s_net_sink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "engine_register_sink failed: %s", esp_err_to_name(err));
        /* Non-fatal: device still scans; WS won't stream data */
    }
    engine_resync(&s_net_sink); /* replay any in-progress scan */

    /* 7. Initial TFT WiFi info (SoftAP; overwritten when STA connects) */
    char url[64];
    snprintf(url, sizeof(url), "http://%s", AP_GATEWAY_IP);
    ui_tft_set_wifi_info(s_ap_ssid, AP_GATEWAY_IP, url);

    ESP_LOGI(TAG, "net_comms ready — http://%s  WS: ws://%s%s",
             AP_GATEWAY_IP, AP_GATEWAY_IP, WS_PATH);
    return ESP_OK;
}
