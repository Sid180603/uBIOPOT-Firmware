/**
 * @file metal_id.h
 * @brief Metal identification and WHO limit lookup for DPV peak interpretation.
 *
 * Maps detected peak potentials to known heavy metal species using characteristic
 * DPV peak windows (in acetate/KCl electrolyte on carbon/bismuth electrode).
 * Also provides WHO drinking-water guideline concentrations.
 *
 * PURE C — no esp_* or FreeRTOS headers. Compiles on both firmware and host.
 *
 * PHASE NOTES:
 *   P4  — Metal identification from peak potential + WHO reference values shown.
 *          slope/intercept = 0 and calibrated = false until P8.
 *   P8  — NVS-loaded slope/intercept overrides the stubs here, enabling real
 *          concentration and SAFE/UNSAFE judgement.
 *   P10 — Peak windows may be made runtime-configurable via NVS.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Metal profile
 * ------------------------------------------------------------------------- */

/**
 * @brief Profile for one heavy-metal species detectable by DPV.
 *
 * Peak windows are typical values in 0.1 M acetate buffer pH 4.5
 * on carbon/bismuth film electrode vs Ag/AgCl reference.
 * Real windows shift with electrolyte / electrode / pH — configurable in P10.
 */
typedef struct {
    const char *symbol;        /**< Element symbol: "Pb", "Cd", etc.          */
    const char *name;          /**< Full name: "Lead", "Cadmium", etc.         */
    float  e_min_mV;           /**< Lower bound of characteristic peak window. */
    float  e_max_mV;           /**< Upper bound of characteristic peak window. */
    float  who_limit_ugL;      /**< WHO drinking-water guideline (µg/L).
                                     0 = no WHO numeric guideline.              */
    /* --- calibration fields (populated in P8 from NVS) --- */
    float  slope_uA_per_uM;    /**< Linear sensitivity (µA per µM).  0 = not calibrated. */
    float  intercept_uA;       /**< Blank/baseline intercept (µA).              */
    bool   calibrated;         /**< True only after P8 bench calibration.       */
} metal_profile_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/**
 * @brief  Return a pointer to the static profile table and its size.
 * @param  count_out  Written with the number of entries.  May be NULL.
 * @return Pointer to the first metal_profile_t entry.
 */
const metal_profile_t *metal_get_profiles(uint8_t *count_out);

/**
 * @brief  Identify the best-matching metal species for a given peak potential.
 *
 * Searches the profile table for the first entry whose [e_min_mV, e_max_mV]
 * window contains E_mV.  Returns NULL if no profile matches (unknown peak).
 *
 * @param  E_mV  Peak potential in millivolts.
 * @return Pointer to the matching metal_profile_t, or NULL.
 */
const metal_profile_t *metal_identify(float E_mV);

/**
 * @brief  Compute concentration from peak current using calibration.
 *
 * C (µM) = (I_peak_uA - intercept_uA) / slope_uA_per_uM
 *
 * Returns NaN (not a number) if profile is NULL or not calibrated,
 * or if the result is negative (below blank).
 *
 * @param  profile    Metal profile (from metal_identify). May be NULL.
 * @param  I_peak_uA  Peak current magnitude (positive).
 * @return Concentration in µM, or NAN if not computable.
 */
float metal_concentration_uM(const metal_profile_t *profile, float I_peak_uA);

#ifdef __cplusplus
}
#endif
