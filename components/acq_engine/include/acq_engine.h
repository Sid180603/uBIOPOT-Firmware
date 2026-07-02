#pragma once

/**
 * @file acq_engine.h
 * @brief Acquisition engine public API — FreeRTOS task infrastructure.
 *
 * Runs on top of echem_core (pure algorithm) and pstat_hal (hardware drivers).
 * Spawns two FreeRTOS tasks:
 *
 *   AcquisitionTask  (Core 1, highest priority)
 *     — sole owner of the HAL (SPI/I2C/GPIO).
 *     — blocks on command queue; on CMD_START runs technique->run() with DI callbacks.
 *     — emits DataPoints + events to the dispatch queue (fast, non-blocking).
 *
 *   DispatcherTask   (Core 0, medium priority)
 *     — drains the dispatch queue.
 *     — appends DataPoints to the server-authoritative scan buffer (under mutex).
 *     — fans out DataPoints and events to all registered sinks.
 *
 * Sinks (TFT=P4, WebSocket=P5, Serial=P7) call engine_register_sink() and receive
 * callbacks from the Dispatcher. Adding a new sink NEVER modifies the engine.
 *
 * Server-authoritative buffer: the engine owns current_scan[].  A late-joining
 * client (phone that slept mid-scan) calls engine_resync() to rebuild the full
 * trace.  The server owns the data; UIs are pure renderers.
 *
 * Abort: engine_abort() sets an atomic flag read by check_abort() every DPV step.
 * Latency < one DPV step (< t_period_ms).
 */

#include "echem_core/scan_state.h"   /* DataPoint, scan_state_t, scan_event_t */
#include "echem_core/calibration.h"  /* pstat_calib_t */
#include "echem_core/dpv.h"          /* dpv_params_t */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Capacity constants
 * -------------------------------------------------------------------------- */

/** Maximum number of simultaneously registered sinks (TFT + WS + Serial = 3). */
#define ENGINE_MAX_SINKS       4

/**
 * Maximum DataPoints in the server-authoritative scan buffer.
 * DPV worst-case: floor(2000 mV / 5 mV) + 1 = 401 pts/cycle.
 * With up to 3 sequential electrodes: 3 × 401 = 1203. Use 1300 for headroom.
 */
#define ENGINE_SCAN_BUF_MAX    1300

/* --------------------------------------------------------------------------
 * Sink interface
 *
 * All callbacks are called from DispatcherTask (Core 0).
 * They MUST be non-blocking — no vTaskDelay, no mutex acquisition that could
 * stall the Dispatcher for a significant time.
 * -------------------------------------------------------------------------- */

typedef struct {
    /**
     * Called once for every emitted DataPoint.
     * May be NULL (sink only interested in events).
     */
    void (*on_point)(const DataPoint *pt, void *ctx);

    /**
     * Called on scan state transitions and notable events.
     * @param evt   The event that caused the transition.
     * @param info  Optional human-readable string (e.g. error message). May be NULL.
     * May be NULL (sink only interested in data).
     */
    void (*on_event)(scan_event_t evt, const char *info, void *ctx);

    /**
     * Called when engine_resync() is invoked for this specific sink.
     * Delivers a snapshot of the full scan buffer and current state so a
     * late-connecting client (e.g. phone that rejoined mid-scan) can rebuild.
     * @param buf    Pointer to the snapshot array (may be NULL if count == 0).
     * @param count  Number of valid DataPoints in buf.
     * @param state  Engine state at the moment of the snapshot.
     * May be NULL (sink does not support resync).
     */
    void (*on_resync)(const DataPoint *buf, uint16_t count,
                      scan_state_t state, void *ctx);

    /** Opaque user context pointer passed to every callback. */
    void *ctx;
} engine_sink_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * @brief  Initialise the acquisition engine.
 *
 * Creates FreeRTOS queues and mutexes, then spawns:
 *   — AcquisitionTask  (Core 1, priority configMAX_PRIORITIES - 1)
 *   — DispatcherTask   (Core 0, priority configMAX_PRIORITIES / 2)
 *
 * @param  cal  Calibration constants used by the HAL callbacks.
 *              The pointer must remain valid for the lifetime of the engine
 *              (use a static or heap-allocated struct in app_main).
 * @return ESP_OK on success; ESP_ERR_NO_MEM if a queue or task could not be
 *         created.
 */
esp_err_t acq_engine_init(pstat_calib_t *cal);

/* --------------------------------------------------------------------------
 * Engine commands  (thread-safe — may be called from any task or ISR context)
 * -------------------------------------------------------------------------- */

/**
 * @brief  Start a DPV scan.
 *
 * Posts CMD_START to the command queue.  AcquisitionTask picks it up, validates
 * params, selects the electrode, runs equilibration + waveform, emits DataPoints.
 *
 * @param  electrode  1, 2, or 3 for a single electrode.
 *                    0 = ELECTRODE_ALL: sequential scan of electrodes 1 → 2 → 3.
 * @param  params     DPV scan parameters (copied into the command struct; caller
 *                    does NOT need to keep the pointer alive after the call).
 * @return ESP_OK
 *         ESP_ERR_INVALID_STATE  engine not IDLE
 *         ESP_ERR_INVALID_ARG    params is NULL
 *         ESP_ERR_TIMEOUT        command queue full (should never happen in practice)
 */
esp_err_t engine_start(uint8_t electrode, const dpv_params_t *params);

/**
 * @brief  Request an abort of the running scan.
 *
 * Sets an atomic flag that the DPV algorithm polls via check_abort() after
 * every step.  Returns immediately (does not block until abort completes).
 * Abort latency < one DPV step (< t_period_ms, typically < 200 ms).
 */
void engine_abort(void);

/**
 * @brief  Request an auto-zero calibration run.
 *
 * Posts CMD_ZERO.  AcquisitionTask measures the TIA quiescent output (WE
 * floating) and stores the result in the calibration struct as current_offset_uA.
 * Must be called while engine is IDLE.
 *
 * @return ESP_OK or ESP_ERR_INVALID_STATE.
 */
esp_err_t engine_zero(void);

/**
 * @brief  Return the current scan state.
 *
 * Lock-free atomic read — safe to call from any context including ISRs.
 */
scan_state_t engine_get_state(void);

/* --------------------------------------------------------------------------
 * Sink management  (thread-safe)
 * -------------------------------------------------------------------------- */

/**
 * @brief  Register a sink to receive DataPoints, events, and resyncs.
 *
 * Sinks are called from DispatcherTask (Core 0).  All callbacks must be
 * non-blocking.  Registered sinks persist until the device reboots.
 *
 * Typical call order:
 *   engine_register_sink(&my_sink);
 *   engine_resync(&my_sink);   // replay any in-progress scan buffer
 *
 * @return ESP_OK or ESP_ERR_NO_MEM (ENGINE_MAX_SINKS exceeded).
 */
esp_err_t engine_register_sink(const engine_sink_t *sink);

/**
 * @brief  Deliver a full snapshot of the scan buffer to one specific sink.
 *
 * Thread-safe: acquires the scan-buffer mutex, takes a consistent snapshot,
 * then calls sink->on_resync().  Use after engine_register_sink() to sync a
 * late-joining client.
 *
 * If sink->on_resync is NULL, this is a no-op.
 */
void engine_resync(const engine_sink_t *sink);

#ifdef __cplusplus
}
#endif

