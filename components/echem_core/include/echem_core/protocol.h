#pragma once

/**
 * @file protocol.h
 * @brief Shared protocol types and NDJSON formatting utilities.
 *
 * The parsing of inbound JSON commands lives in net_comms (P5, using cJSON) and
 * serial_comms (P7). cJSON is intentionally EXCLUDED from echem_core to keep it
 * dependency-free and host-testable.
 *
 * This header defines only:
 *   - The parsed command struct (pstat_cmd_t) shared between all transports
 *   - NDJSON formatting functions (snprintf-based, no cJSON needed for output)
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file.
 */

#include "technique.h"
#include "dpv.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Protocol version
 * Increment UBIOPOT_PROTOCOL_VERSION on any breaking change to NDJSON schema.
 * -------------------------------------------------------------------------- */

#define UBIOPOT_PROTOCOL_VERSION  1
#define UBIOPOT_FW_VERSION_STR    "1.0.0-dev"

/* --------------------------------------------------------------------------
 * Command types (host → device)
 * -------------------------------------------------------------------------- */

typedef enum {
    CMD_START  = 0,  /**< Start a scan. Carries technique name + params. */
    CMD_ABORT  = 1,  /**< Abort the running scan. */
    CMD_ZERO   = 2,  /**< Run auto-zero routine (P8). */
    CMD_STATE  = 3,  /**< Query current engine state. */
    CMD_HELLO  = 4,  /**< Handshake: device responds with fw + proto version. */
} cmd_type_t;

/**
 * @brief  Parsed command from any transport (serial or WebSocket).
 *
 * net_comms (P5) and serial_comms (P7) parse JSON into this struct and pass it
 * to the engine command queue. The engine never sees raw JSON.
 */
typedef struct {
    cmd_type_t   type;
    char         technique[16];  /**< "DPV" etc. Only valid for CMD_START. */
    dpv_params_t dpv;            /**< DPV params. Only valid when type==CMD_START and
                                      technique=="DPV". Other techniques added post-publish. */
} pstat_cmd_t;

/* --------------------------------------------------------------------------
 * NDJSON output formatters (device → host)
 * All return the number of bytes written (excluding '\0'), or -1 on truncation.
 * Each line ends with '\n' as required by NDJSON (newline-delimited JSON).
 * -------------------------------------------------------------------------- */

/**
 * @brief  Format a data point:
 *   {"t":"point","e":<electrode>,"n":<idx>,"v":<E_V>,"i":<I_uA>,"re":<RE_V>}
 * x-axis: E_mV → E_V (divided by 1000).
 */
int protocol_format_point(char *buf, size_t buf_len,
                          uint8_t electrode, uint16_t idx,
                          float E_mV, float I_uA, float RE_mV);

/**
 * @brief  Format scan_started event:
 *   {"t":"event","name":"scan_started","mode":"DPV","e":<electrode>}
 */
int protocol_format_event_started(char *buf, size_t buf_len,
                                  const char *technique, uint8_t electrode);

/**
 * @brief  Format scan_complete event:
 *   {"t":"event","name":"scan_complete"}
 */
int protocol_format_event_complete(char *buf, size_t buf_len);

/**
 * @brief  Format scan_error event:
 *   {"t":"event","name":"scan_error","msg":"<message>"}
 */
int protocol_format_event_error(char *buf, size_t buf_len, const char *msg);

/**
 * @brief  Format hello/handshake response:
 *   {"t":"hello","fw":"1.0.0-dev","proto":1}
 */
int protocol_format_hello(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
