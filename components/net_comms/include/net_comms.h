#pragma once

/**
 * @file net_comms.h
 * @brief WiFi connectivity, HTTP server, and WebSocket streaming — public API (P5).
 *
 * WiFi APSTA:
 *   SoftAP always on: SSID "Aqua-HMET-<MAC4>", WPA2, GW 192.168.4.1.
 *   STA optional: NVS credentials → connect + auto-reconnect.
 *   Captive portal: UDP:53 DNS answers every domain → 192.168.4.1.
 *   mDNS: hostname CONFIG_UBIOPOT_MDNS_HOSTNAME → <host>.local (_http._tcp).
 *
 * HTTP server (Core-0, esp_http_server):
 *   GET  /               → LittleFS SPA (gzip-transparent, chunked)
 *   GET  /api/state      → JSON scan state
 *   POST /api/scan       → start DPV (JSON body with params)
 *   POST /api/abort      → abort running scan
 *   GET  /api/scan.csv   → current scan as CSV download
 *   POST /api/reference.csv → upload reference CSV (stored on LittleFS)
 *   POST /api/wifi       → provision STA credentials (NVS)
 *   WS   /ws             → binary DataPoints + JSON events; inbound START/ABORT/ZERO
 *
 * WS broadcast: ALL sends via httpd_queue_work → httpd_ws_send_frame_async
 * (httpd is NOT thread-safe; Dispatcher MUST NOT call httpd directly).
 *
 * Security: NO TLS (plain HTTP on local SoftAP link). Web Serial standalone
 * page hosted on GitHub Pages (HTTPS) for secure-context requirement (P9).
 *
 * Wires WiFi info to P4 TFT: calls ui_tft_set_wifi_info() after AP/STA ready.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise and start all network services.
 *
 * Sequence:
 *   1. Mount LittleFS partition (web assets).
 *   2. Start WiFi APSTA (SoftAP always on; STA if NVS credentials present).
 *   3. Start captive-portal DNS responder task.
 *   4. Start mDNS.
 *   5. Start HTTP server + WebSocket endpoint.
 *   6. Register as engine sink (DataPoints + events → WS broadcast).
 *   7. Call ui_tft_set_wifi_info() with real SSID / IP / URL.
 *
 * Must be called after acq_engine_init() and ui_tft_start().
 * Runs entirely on Core 0 (httpd task affinity = Core 0).
 *
 * @return ESP_OK on success.  HTTP/WS services continue running as background
 *         tasks; this call returns as soon as the server is started.
 */
esp_err_t net_comms_start(void);

#ifdef __cplusplus
}
#endif
