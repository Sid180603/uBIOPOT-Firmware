#pragma once

/**
 * @file serial_comms_protocol.h
 * @brief P7 USB-serial (UART0) NDJSON wire-protocol definitions — pure C, no IDF headers.
 *
 * This header is intentionally IDF-free so it can be included in:
 *   - serial_comms.c  (firmware, ESP-IDF component)
 *   - host_test/test_serial_protocol.c  (host GCC, Unity tests)
 *   - test/test_p7_serial.py  (Python pytest, reads constants via string matching)
 *
 * Wire protocol summary
 * ─────────────────────
 * Transport  : UART0 (GPIO1/TX, GPIO3/RX), 115200 baud default (Kconfig-configurable).
 * Framing    : NDJSON — one JSON object per line, terminated with '\n'.
 * Direction  : host→device = commands; device→host = output messages.
 * Versioning : proto field in hello message; bump SERIAL_PROTOCOL_VERSION on breaking changes.
 *
 * LOG ROUTING (Option A per plan):
 *   esp_log_level_set("*", ESP_LOG_WARN) at init → suppresses INFO/DEBUG logs that
 *   would otherwise corrupt the NDJSON stream.  Host parser MUST silently ignore
 *   any line that does not start with '{' (boot logs, stray output).
 *
 * PARITY TABLE (serial NDJSON <-> WebSocket JSON, P5/P6):
 * ─────────────────────────────────────────────────────────────────────────────
 *  Serial NDJSON line                       │ WS equivalent
 * ─────────────────────────────────────────────────────────────────────────────
 *  {"t":"hello","fw":"1.0.0","proto":1,...} │ {"t":"hello","fw":"1.0.0","proto":1,...}
 *  {"t":"point","e":1,"idx":0,"E":0.21,...} │ binary 16-byte ws_dp_frame_t (same semantics)
 *  {"t":"event","name":"scan_started",...}  │ {"t":"event","name":"scan_started",...}
 *  {"t":"event","name":"scan_complete"}     │ {"t":"event","name":"scan_complete"}
 *  {"t":"event","name":"scan_aborted"}      │ {"t":"event","name":"scan_aborted"}
 *  {"t":"event","name":"scan_error","msg"}  │ {"t":"event","name":"scan_error","msg"}
 *  {"t":"state","state":0}                  │ {"t":"state","state":0}
 *  {"t":"resync_complete","count":N,...}    │ {"t":"resync_complete","pts":N,"state":S}
 *  {"t":"error","msg":"..."}                │ {"t":"error","msg":"..."}
 * ─────────────────────────────────────────────────────────────────────────────
 *  Host→Device: {"cmd":"start","electrode":1,"params":{...}} │ WS same JSON object
 *  Host→Device: {"cmd":"abort"}                              │ WS same
 *  Host→Device: {"cmd":"zero"}                               │ WS same
 *  Host→Device: {"cmd":"state"}                              │ WS same
 *  Host→Device: {"cmd":"hello"}                              │ WS same
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Key difference: serial uses NDJSON TEXT for all messages (debuggable);
 * WS uses BINARY frames for DataPoints (bandwidth efficiency).
 * Semantics are identical — same engine, same state machine.
 *
 * This header is the single source of truth for the serial wire format.
 * serial_comms.c includes it; test_serial_protocol.c (host) includes it.
 * test_p7_serial.py mirrors these constants on the Python side.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Protocol versioning
 * -------------------------------------------------------------------------- */

/** Bump on any breaking change to the NDJSON schema. */
#define SERIAL_PROTOCOL_VERSION   1

/** Firmware version string embedded in hello message. */
#define SERIAL_FW_VERSION_STR     "1.0.0"

/** Device name string embedded in hello message. */
#define SERIAL_DEVICE_NAME        "Aqua-HMET"

/* --------------------------------------------------------------------------
 * NDJSON output message type field (value of "t" key)
 * -------------------------------------------------------------------------- */

#define SERIAL_T_HELLO            "hello"
#define SERIAL_T_POINT            "point"
#define SERIAL_T_EVENT            "event"
#define SERIAL_T_STATE            "state"
#define SERIAL_T_PEAKS            "peaks"
#define SERIAL_T_RESYNC_COMPLETE  "resync_complete"
#define SERIAL_T_ERROR            "error"

/* --------------------------------------------------------------------------
 * Event "name" field values (used when "t":"event")
 * Must match net_comms_protocol.h NET_EVT_NAME_* for parity.
 * -------------------------------------------------------------------------- */

#define SERIAL_EVT_NAME_STARTED   "scan_started"
#define SERIAL_EVT_NAME_EQUILIB   "equilibrating"
#define SERIAL_EVT_NAME_COMPLETE  "scan_complete"
#define SERIAL_EVT_NAME_ABORTED   "scan_aborted"
#define SERIAL_EVT_NAME_ERROR     "scan_error"
#define SERIAL_EVT_NAME_RESET     "scan_reset"

/* --------------------------------------------------------------------------
 * Inbound command names (value of "cmd" key, host→device)
 * Must match net_comms_protocol.h NET_CMD_* for parity.
 * -------------------------------------------------------------------------- */

#define SERIAL_CMD_START   "start"
#define SERIAL_CMD_ABORT   "abort"
#define SERIAL_CMD_ZERO    "zero"
#define SERIAL_CMD_STATE   "state"
#define SERIAL_CMD_HELLO   "hello"
#define SERIAL_CMD_NAV     "nav"    /* drive the TFT: {"cmd":"nav","screen":"settings"} */

/* --------------------------------------------------------------------------
 * DPV parameter keys (inside "params":{} of the start command)
 * Must match the dpv_params_t field names used in net_comms.c for parity.
 * -------------------------------------------------------------------------- */

#define SERIAL_PARAM_E_BEGIN_MV         "e_begin_mV"
#define SERIAL_PARAM_E_END_MV           "e_end_mV"
#define SERIAL_PARAM_E_STEP_MV          "e_step_mV"
#define SERIAL_PARAM_E_PULSE_MV         "e_pulse_mV"
#define SERIAL_PARAM_T_PULSE_MS         "t_pulse_ms"
#define SERIAL_PARAM_T_PERIOD_MS        "t_period_ms"
#define SERIAL_PARAM_T_EQUILIBRATION_MS "t_equilibration_ms"
#define SERIAL_PARAM_CYCLES             "cycles"
#define SERIAL_PARAM_N_AVG              "n_avg"

/* --------------------------------------------------------------------------
 * Point output field names  (keys in {"t":"point",...})
 * -------------------------------------------------------------------------- */

#define SERIAL_POINT_T         "t"
#define SERIAL_POINT_ELECTRODE "e"
#define SERIAL_POINT_IDX       "idx"
#define SERIAL_POINT_E_MV      "E"
#define SERIAL_POINT_I_UA      "I"
#define SERIAL_POINT_RE_MV     "RE"

/* --------------------------------------------------------------------------
 * Hello output field names
 * -------------------------------------------------------------------------- */

#define SERIAL_HELLO_T        "t"
#define SERIAL_HELLO_FW       "fw"
#define SERIAL_HELLO_PROTO    "proto"
#define SERIAL_HELLO_DEVICE   "device"

/* --------------------------------------------------------------------------
 * NDJSON line limits
 * -------------------------------------------------------------------------- */

/** Maximum bytes in one inbound JSON command line (host→device). */
#define SERIAL_CMD_LINE_MAX    512u

/** Maximum bytes in one outbound NDJSON point line (device→host).
 *  {"t":"point","e":3,"idx":65535,"E":-1000.0000,"I":-9999.9999,"RE":-2000.0000}
 *  is ~80 chars — 128 is safe headroom. */
#define SERIAL_POINT_LINE_MAX  128u

/** Bytes read per uart_read_bytes() call in the RX task.
 *  64 bytes = ~half a typical NDJSON command — good balance of latency vs. cost. */
#define SERIAL_CHUNK_BYTES     64u

/** Default UART baud rate.  Exposed in Kconfig as CONFIG_UBIOPOT_SERIAL_BAUD. */
#define SERIAL_DEFAULT_BAUD    115200u

#ifdef __cplusplus
}
#endif
