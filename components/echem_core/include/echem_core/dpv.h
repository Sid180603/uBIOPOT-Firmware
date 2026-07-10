#pragma once

/**
 * @file dpv.h
 * @brief Differential Pulse Voltammetry technique — parameters and descriptor.
 *
 * DPV canon (PalmSens): constant-amplitude pulses (E_pulse) on a DC staircase.
 * Current sampled TWICE per step:
 *   I_base  = average of n_avg samples at END of baseline period (transients decayed)
 *   I_pulse = average of n_avg samples at END of pulse period
 *   dI = I_pulse - I_base  (the output data point — background-suppressed)
 *
 * X-axis = BASE potential E (not the mean E + E_pulse/2).
 * Note: E_peak appears shifted from E_half by E_pulse/2 — annotated in P10.
 *
 * This is the default technique on power-on (technique_registry_init registers it first).
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file.
 */

#include "technique.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * DPV scan parameters — all in REAL PHYSICAL UNITS (not DAC counts)
 * -------------------------------------------------------------------------- */

typedef struct {
    float    e_begin_mV;           /**< Start potential (mV). Range: ±1000 mV. */
    float    e_end_mV;             /**< End potential (mV).   Range: ±1000 mV. */
    float    e_step_mV;            /**< Step increment (mV). Must be > 0. Typical: 5 mV. */
    float    e_pulse_mV;           /**< Pulse amplitude (mV). Must be > 0. Typical: 25 mV.
                                        Direction follows scan: +e_pulse for anodic. */
    uint32_t t_pulse_ms;           /**< Pulse duration (ms). Must satisfy: t_pulse < t_period. */
    uint32_t t_period_ms;          /**< Step period (ms) = baseline duration + pulse duration.
                                        Must be > t_pulse_ms. */
    uint32_t t_equilibration_ms;   /**< Hold at e_begin before sweep starts (ms).
                                        Lets the electrode equilibrate. Typical: 1000–5000 ms. */
    uint8_t  cycles;               /**< Number of forward sweeps (integer ≥ 1). */
    uint8_t  n_avg;                /**< ADC samples to average per baseline/pulse read.
                                        Default 5 → √5 noise reduction (~2.2×). */
    electrode_t electrode;         /**< Electrode to use: ELECTRODE_1/2/3 or ELECTRODE_ALL
                                        (sequential 1→2→3). */
} dpv_params_t;

/* --------------------------------------------------------------------------
 * DPV default parameters (OP07 board, safe starting point)
 * -------------------------------------------------------------------------- */

#define DPV_PARAMS_DEFAULT {                    \
    .e_begin_mV         = -900.0f,              \
    .e_end_mV           =  500.0f,              \
    .e_step_mV          =    5.0f,              \
    .e_pulse_mV         =   25.0f,              \
    .t_pulse_ms         =   50,                 \
    .t_period_ms        =  200,                 \
    .t_equilibration_ms = 2000,                 \
    .cycles             =    1,                 \
    .n_avg              =    5,                 \
    .electrode          = ELECTRODE_1,          \
}

/* --------------------------------------------------------------------------
 * Technique descriptor accessor
 * -------------------------------------------------------------------------- */

/**
 * @brief  Return a pointer to the DPV technique descriptor.
 *         Called by technique_registry_init() to register DPV.
 */
const technique_t *dpv_get_technique(void);

#ifdef __cplusplus
}
#endif
