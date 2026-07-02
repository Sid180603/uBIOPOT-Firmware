#pragma once

/**
 * @file scan_state.h
 * @brief Pure electrochemical scan state machine — types and transition function.
 *
 * DataPoint, scan_state_t, scan_event_t and scan_state_next() are defined here so
 * they can be used by:
 *   - acq_engine (IDF component, FreeRTOS) — the live task infrastructure.
 *   - host_test  (host GCC, no IDF)        — unit tests on the PC without a board.
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file. Purity enforced by the
 * host_test build: any accidental IDF include would break compilation there.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * DataPoint — one emitted sample from the electrochemistry algorithm.
 *
 * Carried through the FreeRTOS datapoint queue (acq_engine) and stored in the
 * server-authoritative scan buffer.  Sinks (TFT, WebSocket, serial) consume
 * these via on_point() callbacks.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  electrode;  /**< Active electrode: 1, 2, or 3. */
    uint16_t idx;        /**< Step index within the current scan (0-based). */
    float    E_mV;       /**< Base potential (mV) — x-axis value. */
    float    I_uA;       /**< dI = I_pulse − I_base (µA) — y-axis value. */
    float    RE_mV;      /**< Measured RE voltage (mV) — 3rd column for iR correction. */
} DataPoint;

/* --------------------------------------------------------------------------
 * scan_state_t — state of the acquisition engine.
 *
 * Transitions are driven by scan_event_t via scan_state_next().
 * Only scan_state_next() is allowed to produce a next-state — no ad-hoc
 * state overrides anywhere in the codebase.
 * -------------------------------------------------------------------------- */

typedef enum {
    SCAN_STATE_IDLE          = 0, /**< No scan running. Ready for CMD_START. */
    SCAN_STATE_EQUILIBRATING = 1, /**< Holding at e_begin; equilibration delay pending. */
    SCAN_STATE_RUNNING       = 2, /**< Actively stepping through the waveform. */
    SCAN_STATE_COMPLETE      = 3, /**< Scan finished normally (auto-resets to IDLE). */
    SCAN_STATE_ABORTING      = 4, /**< Abort requested; waiting for algorithm to stop. */
    SCAN_STATE_ERROR         = 5, /**< Fatal scan error occurred (auto-resets to IDLE). */
} scan_state_t;

/* --------------------------------------------------------------------------
 * scan_event_t — events that drive state transitions.
 * -------------------------------------------------------------------------- */

typedef enum {
    SCAN_EVT_START,        /**< Valid CMD_START received; begin scan sequence.         */
    SCAN_EVT_EQUILIB_DONE, /**< Equilibration done; start waveform steps.             */
    SCAN_EVT_SCAN_DONE,    /**< technique->run() returned 0 — normal completion.       */
    SCAN_EVT_ABORTED,      /**< technique->run() returned +1 — abort was polled.       */
    SCAN_EVT_ERROR,        /**< technique->run() returned negative, or param invalid.  */
    SCAN_EVT_RESET,        /**< Return to IDLE (user ack or auto after terminal state). */
} scan_event_t;

/* --------------------------------------------------------------------------
 * scan_state_next — pure deterministic state-transition function.
 *
 * Returns the next state given the current state and event.
 * If the (state, event) pair has no defined transition, returns the CURRENT
 * state unchanged — no silent corruption.
 *
 * This function is the ONLY place that encodes the transition table, making
 * it a single-source-of-truth that is cheaply host-unit-tested.
 * -------------------------------------------------------------------------- */

scan_state_t scan_state_next(scan_state_t current, scan_event_t event);

#ifdef __cplusplus
}
#endif
