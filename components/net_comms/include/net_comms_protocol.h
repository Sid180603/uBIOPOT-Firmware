#pragma once

/**
 * @file net_comms_protocol.h
 * @brief P5 WebSocket + REST wire-protocol definitions — pure C, no IDF headers.
 *
 * This header is intentionally IDF-free so it can be included in host_test/
 * (host GCC, no ESP-IDF toolchain) to enable protocol conformance tests.
 *
 * Wire protocol summary
 * ─────────────────────
 * WS /ws binary frames  : DataPoint, 16 bytes, little-endian (see ws_dp_frame_t)
 * WS /ws text frames    : JSON events/state/resync (see event name constants below)
 * REST CSV              : electrode,idx,E_mV,I_uA,RE_mV\r\n rows
 * REST JSON             : all API responses use MIME application/json
 *
 * This header is the single source of truth for the on-wire layout.
 * net_comms.c includes it; test_net_comms.c (host) includes it.
 * potentiostat-core.js (P6 SPA) mirrors this layout on the JS side.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Protocol version — bump on any breaking wire-format change.
 * Carried in the WS hello JSON message: {"t":"hello","proto":1,...}
 * -------------------------------------------------------------------------- */
#define NET_PROTOCOL_VERSION    1
#define NET_FW_VERSION_STR      "1.0.0"

/* --------------------------------------------------------------------------
 * WS binary DataPoint frame — 16 bytes, little-endian, packed.
 *
 * Byte  0   : frame_type  (always NET_WS_FRAME_TYPE_DATAPOINT = 0x01)
 * Byte  1   : electrode   (uint8,  1/2/3)
 * Bytes 2-3 : idx         (uint16 LE, step index within scan)
 * Bytes 4-7 : E_mV        (float32 LE, base potential, mV)
 * Bytes 8-11: I_uA        (float32 LE, dI = I_pulse − I_base, µA)
 * Bytes12-15: RE_mV       (float32 LE, measured RE voltage, mV)
 *
 * The JS DataView parse pattern (potentiostat-core.js, P6):
 *   const type  = dv.getUint8(0);         // must equal 0x01
 *   const elec  = dv.getUint8(1);
 *   const idx   = dv.getUint16(2, true);  // LE
 *   const E_mV  = dv.getFloat32(4, true);
 *   const I_uA  = dv.getFloat32(8, true);
 *   const RE_mV = dv.getFloat32(12, true);
 * -------------------------------------------------------------------------- */

#define NET_WS_FRAME_TYPE_DATAPOINT  0x01u
#define NET_WS_FRAME_SIZE            16u   /* sizeof(net_ws_dp_frame_t) must equal this */

typedef struct __attribute__((packed)) {
    uint8_t  frame_type;   /**< NET_WS_FRAME_TYPE_DATAPOINT */
    uint8_t  electrode;    /**< Active electrode: 1, 2, or 3 */
    uint16_t idx;          /**< Step index (0-based) */
    float    E_mV;         /**< Base potential (mV) */
    float    I_uA;         /**< dI = I_pulse − I_base (µA) */
    float    RE_mV;        /**< RE voltage (mV) */
} net_ws_dp_frame_t;

/* --------------------------------------------------------------------------
 * CSV wire format constants
 *
 * GET /api/scan.csv produces:
 *   <NET_CSV_HEADER>
 *   <electrode>,<idx>,<E_mV>,<I_uA>,<RE_mV>\r\n
 *   ...
 * -------------------------------------------------------------------------- */

#define NET_CSV_HEADER      "electrode,idx,E_mV,I_uA,RE_mV\r\n"
#define NET_CSV_ROW_FMT     "%u,%u,%.4f,%.4f,%.4f\r\n"
#define NET_CSV_MIME        "text/csv"
#define NET_CSV_DISPOSITION "attachment; filename=\"aquahmet_scan.csv\""

/* --------------------------------------------------------------------------
 * WS JSON event type strings  (value of "t" field)
 * -------------------------------------------------------------------------- */

#define NET_EVT_HELLO           "hello"
#define NET_EVT_EVENT           "event"
#define NET_EVT_STATE           "state"
#define NET_EVT_RESYNC_COMPLETE "resync_complete"
#define NET_EVT_ERROR           "error"

/* Event "name" field values (used when "t":"event") */
#define NET_EVT_NAME_STARTED    "scan_started"
#define NET_EVT_NAME_EQUILIB    "equilibrating"
#define NET_EVT_NAME_COMPLETE   "scan_complete"
#define NET_EVT_NAME_ABORTED    "scan_aborted"
#define NET_EVT_NAME_ERROR      "scan_error"
#define NET_EVT_NAME_RESET      "scan_reset"

/* --------------------------------------------------------------------------
 * WS inbound command names  (value of "cmd" field, host→device)
 * -------------------------------------------------------------------------- */

#define NET_CMD_START   "start"
#define NET_CMD_ABORT   "abort"
#define NET_CMD_ZERO    "zero"
#define NET_CMD_STATE   "state"
#define NET_CMD_HELLO   "hello"

/* --------------------------------------------------------------------------
 * REST API paths
 * -------------------------------------------------------------------------- */

#define NET_API_STATE        "/api/state"
#define NET_API_SCAN         "/api/scan"
#define NET_API_ABORT        "/api/abort"
#define NET_API_CSV          "/api/scan.csv"
#define NET_API_REFERENCE    "/api/reference.csv"
#define NET_API_WIFI         "/api/wifi"
#define NET_WS_PATH          "/ws"

/* --------------------------------------------------------------------------
 * MIME type helpers (pure logic — re-used by net_comms.c and host_test)
 * -------------------------------------------------------------------------- */

/** @brief True if string s ends with suffix. Both must be non-NULL. */
static inline int net_ends_with(const char *s, const char *suffix)
{
    size_t slen = 0, xlen = 0;
    const char *p = s;      while (*p) { slen++; p++; }
    const char *q = suffix; while (*q) { xlen++; q++; }
    if (slen < xlen) return 0;
    const char *tail = s + slen - xlen;
    while (*suffix) {
        if (*tail != *suffix) return 0;
        tail++; suffix++;
    }
    return 1;
}

/** @brief Return MIME type string for a filepath (based on extension). */
static inline const char *net_content_type(const char *path)
{
    if (net_ends_with(path, ".html")) return "text/html";
    if (net_ends_with(path, ".css"))  return "text/css";
    if (net_ends_with(path, ".js"))   return "application/javascript";
    if (net_ends_with(path, ".json")) return "application/json";
    if (net_ends_with(path, ".ico"))  return "image/x-icon";
    if (net_ends_with(path, ".png"))  return "image/png";
    if (net_ends_with(path, ".svg"))  return "image/svg+xml";
    if (net_ends_with(path, ".csv"))  return "text/csv";
    if (net_ends_with(path, ".gz"))   return "application/gzip";
    return "text/plain";
}

/** @brief Return 1 if the URI path contains a traversal sequence (".."). */
static inline int net_path_is_traversal(const char *uri)
{
    /* Walk the string searching for ".." to avoid false positives from
     * strstr on paths like "/endpoint.." that don't actually traverse. */
    while (*uri) {
        if (uri[0] == '.' && uri[1] == '.') return 1;
        uri++;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
