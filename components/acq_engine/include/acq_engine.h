#pragma once

/**
 * @file acq_engine.h
 * @brief Acquisition engine public API.
 *
 * TODO P3: implement the following API.
 *
 * Architecture:
 *   - AcquisitionTask (Core-1, HIGH priority) = sole owner of pstat_hal.
 *     Runs the technique->run() with DI callbacks. Emits DataPoints to queue.
 *   - Dispatcher task (Core-0, lower priority) drains the queue → appends
 *     server-authoritative scan buffer → fan-out to registered sinks.
 *   - Sinks: TFT (P4), WebSocket broadcaster (P5), serial printer (P7).
 *     Added via engine_register_sink() without touching the engine.
 *
 * ScanState machine:
 *   IDLE → EQUILIBRATING → RUNNING → COMPLETE
 *                                  ↘ ABORTING → IDLE
 *                                  ↘ ERROR    → IDLE
 */

/* TODO P3: engine_start(), engine_abort(), engine_zero(),
 *          engine_get_state(), engine_register_sink(), engine_resync() */
