#pragma once

/**
 * @file peaks.h
 * @brief Simple prominence-filtered peak finder for on-device TFT display.
 *
 * Pure C, no dependencies. Sufficient for the TFT results screen (P4).
 * Heavier peak analysis (scipy server-side, JS peak detection in SPA) lives in the UIs (P6).
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A single detected peak. */
typedef struct {
    float    E_mV;    /**< Potential at peak (mV). */
    float    I_uA;    /**< Peak current (µA). Positive = anodic peak. */
    uint16_t index;   /**< Index into the data arrays passed to peaks_find(). */
} peak_t;

/**
 * @brief  Find local maxima (anodic peaks) in a differential-current array.
 *
 * Algorithm: local maximum with prominence ≥ prominence_threshold_uA.
 * Prominence = peak value minus the highest minimum between this peak and any
 * taller neighbour (simple valley-floor measure for embedded use).
 *
 * @param  I_uA                   Array of dI values (µA), length n.
 * @param  E_mV                   Array of potential values (mV), same length as I_uA.
 * @param  n                      Number of data points.
 * @param  out                    Output array for detected peaks. Caller provides storage.
 * @param  max_peaks              Capacity of out[].
 * @param  prominence_threshold   Minimum prominence (µA) to qualify as a peak.
 * @return                        Number of peaks written into out[] (≤ max_peaks).
 */
uint16_t peaks_find(const float *I_uA, const float *E_mV, uint16_t n,
                    peak_t *out, uint16_t max_peaks, float prominence_threshold);

#ifdef __cplusplus
}
#endif
