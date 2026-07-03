#pragma once

/**
 * @file net_comms.h
 * @brief WiFi connectivity and WebSocket streaming public API.
 *
 * TODO P5: implement the following.
 *
 * WiFi APSTA:
 *   SoftAP always on: SSID "Aqua-HMET-<MAC4>", WPA2, GW 192.168.4.1.
 *   STA optional: NVS credentials → connect + auto-reconnect.
 *   Captive portal: UDP:53 DNS server answers every domain → 192.168.4.1.
 *   mDNS: hostname "aquahmet" → aquahmet.local (_http._tcp).
 *
 * HTTP server (Core-0, esp_http_server):
 *   GET  /         → LittleFS gzip SPA (chunked, Content-Encoding: gzip)
 *   GET  /api/state
 *   POST /api/scan
 *   POST /api/abort
 *   GET  /api/scan.csv
 *   POST /api/reference.csv
 *   POST /api/wifi
 *   WS   /ws       → binary DataPoints + JSON events; inbound START/ABORT/ZERO
 *
 * WS broadcast: ALL sends via httpd_queue_work → httpd_ws_send_frame_async
 * (httpd is NOT thread-safe; Dispatcher must NOT call httpd directly).
 *
 * Security: NO TLS (plain HTTP on local SoftAP link). Web Serial standalone page
 * hosted on GitHub Pages (HTTPS) for the secure-context requirement (P9).
 */

/* TODO P5: net_comms_start() */
