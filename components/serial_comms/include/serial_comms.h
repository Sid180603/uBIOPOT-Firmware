#pragma once

/**
 * @file serial_comms.h
 * @brief P7 — USB-serial NDJSON protocol: UART0 init, RX command task, engine sink.
 *
 * serial_comms registers as an acquisition engine sink (same pattern as
 * ui_tft / net_comms).  It owns UART0 in parallel with the ESP-IDF console
 * and uses log-level routing to keep the protocol stream clean.
 *
 * Architecture:
 *   serial_comms_start()
 *     ├── UART0 init (115200, 8N1, GPIO1/TX GPIO3/RX)
 *     ├── esp_log_level_set("*", ESP_LOG_WARN)  — suppress INFO/DEBUG on UART0
 *     ├── engine_register_sink(&s_serial_sink)
 *     ├── Emit hello JSON to signal readiness
 *     └── xTaskCreate(serial_rx_task, Core 0)
 *
 *   serial_rx_task (Core 0, low-medium priority):
 *     Reads lines from UART0 RX ring buffer.
 *     Ignores lines that don't start with '{' (e.g. stray boot logs).
 *     Parses JSON commands → engine_start / engine_abort / engine_zero / state / hello.
 *
 *   Engine sink callbacks (called from DispatcherTask, Core 0):
 *     on_point  → NDJSON line: {"t":"point","e":1,"idx":0,"E":-0.5,"I":6.42,"RE":0.93}
 *     on_event  → NDJSON line: {"t":"event","name":"scan_started"}
 *     on_resync → replay all buffered DataPoints + {"t":"resync_complete",...}
 *
 * Log routing rationale (Option A from P7 plan):
 *   UART0 is shared with ESP-IDF console logs.  Setting log level to WARN
 *   suppresses INFO/DEBUG output that would corrupt NDJSON parsing on the host.
 *   ERROR/WARN logs can still appear (prefixed with 'E ' or 'W ') — the host
 *   parser MUST silently ignore any line not starting with '{'.
 *
 * Thread safety:
 *   TX writes are protected by a mutex so point/event/resync lines never interleave.
 *   uart_read_bytes / uart_write_bytes are internally buffered by the IDF driver.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise and start the serial communications subsystem.
 *
 * Configures UART0, reduces log verbosity to WARN, registers an engine
 * sink, emits the hello handshake, and spawns the RX task.
 *
 * Call from app_main after acq_engine_init().  Non-fatal if the engine
 * is not yet running (commands will still be accepted; scan output will
 * stream once a scan starts).
 *
 * @return ESP_OK on success.
 *         ESP_ERR_NO_MEM if task or mutex creation fails.
 *         ESP_FAIL if UART driver installation fails.
 */
esp_err_t serial_comms_start(void);

#ifdef __cplusplus
}
#endif
