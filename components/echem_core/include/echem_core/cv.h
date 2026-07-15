#pragma once

/**
 * @file cv.h
 * @brief Cyclic Voltammetry technique — parameters and descriptor.
 *
 * CV sweeps the potential linearly (staircase approximation) from a start
 * potential to a first vertex, to a second vertex, and back, for N cycles,
 * sampling the RAW current at each small step (no differential — unlike DPV).
 *
 * Output data point: (E = staircase potential mV, I = measured current uA,
 * RE = measured reference potential mV).  A reversible couple shows anodic +
 * cathodic peaks ~59 mV apart; the large steady-state current (vs DPV's small
 * differential) makes CV the high-SNR bring-up/diagnostic technique.
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file (pure-C echem_core invariant).
 */

#include "technique.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * CV scan parameters — all in REAL PHYSICAL UNITS
 * -------------------------------------------------------------------------- */

typedef struct {
    float    e_begin_mV;         /**< Start (and return) potential (mV). ±1000 mV. */
    float    e_vertex1_mV;       /**< First reversal potential (mV). ±1000 mV. */
    float    e_vertex2_mV;       /**< Second reversal potential (mV); usually == e_begin_mV. */
    float    e_step_mV;          /**< Staircase resolution (mV). Must be > 0. Typical 5 mV. */
    float    scan_rate_mV_s;     /**< Sweep rate (mV/s). Must be > 0. Typical 50 mV/s. */
    uint32_t t_equilibration_ms; /**< Hold at e_begin before the sweep starts (ms). */
    uint8_t  cycles;             /**< Number of full CV cycles (>= 1). */
    uint8_t  n_avg;              /**< ADC samples averaged per point. */
    electrode_t electrode;       /**< ELECTRODE_1/2/3 or ELECTRODE_ALL. */
} cv_params_t;

/* --------------------------------------------------------------------------
 * CV default parameters — ferricyanide bring-up window on Ag/AgCl
 * -------------------------------------------------------------------------- */

#define CV_PARAMS_DEFAULT {                 \
    .e_begin_mV         = -200.0f,          \
    .e_vertex1_mV       =  600.0f,          \
    .e_vertex2_mV       = -200.0f,          \
    .e_step_mV          =    5.0f,          \
    .scan_rate_mV_s     =   50.0f,          \
    .t_equilibration_ms = 2000,             \
    .cycles             =    2,             \
    .n_avg              =    4,             \
    .electrode          = ELECTRODE_3,      \
}

/**
 * @brief  Return a pointer to the CV technique descriptor.
 *         Called by technique_registry_init() to register CV.
 */
const technique_t *cv_get_technique(void);

#ifdef __cplusplus
}
#endif
