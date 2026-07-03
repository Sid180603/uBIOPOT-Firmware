/**
 * @file acq_engine.h  (PC simulator stub)
 * Declares only what the screen files (scr_home.c, scr_scan.c) actually call.
 * The implementations live in main_sim.c (engine_start, engine_abort stubs).
 *
 * scan_state_t, DataPoint are pulled in from echem_core/scan_state.h — that
 * file is pure C with no IDF deps, so it compiles fine on the host.
 */
#pragma once

/* Pull in the pure echem types (scan_state_t, DataPoint, scan_event_t).
 * Point the compiler at sim/stubs/ AND components/echem_core/include/
 * in CMakeLists.txt so this include resolves. */
#include "echem_core/scan_state.h"
#include "echem_core/dpv.h"

#include "esp_err.h"   /* esp_err_t stub */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start a DPV scan on the given electrode.
 * Implemented as a synthetic scan in main_sim.c.
 * Return type is esp_err_t (= int) to match the real firmware declaration.
 */
esp_err_t engine_start(uint8_t electrode, const dpv_params_t *params);

/** Abort a running scan. */
void engine_abort(void);

/** Get current engine state (sim always returns IDLE unless scanning). */
scan_state_t engine_get_state(void);

#ifdef __cplusplus
}
#endif
