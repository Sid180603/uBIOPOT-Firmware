#include "echem_core/peaks.h"
#include <float.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal: compute simple prominence for a single candidate peak at index i.
 *
 * Prominence = I_uA[i] - highest_valley_floor
 *
 * The valley floor for peak i is the minimum value in the region between peak i
 * and the nearest taller peak on each side (or the array boundary if none exists).
 * "Highest" means we take the larger of the two valley floors (left and right),
 * because that is the key base from which the peak rises.
 *
 * This is an embedded-friendly O(n) scan per candidate.
 * -------------------------------------------------------------------------- */
static float compute_prominence(const float *I_uA, uint16_t n, uint16_t peak_idx)
{
    float peak_val = I_uA[peak_idx];

    /* --- Left valley floor: min between peak_idx and the nearest taller peak to the left --- */
    float left_valley = peak_val; /* start at peak value; scan will only decrease */
    for (int j = (int)peak_idx - 1; j >= 0; j--) {
        if (I_uA[j] < left_valley) left_valley = I_uA[j];
        if (I_uA[j] >= peak_val)   break; /* hit a taller neighbour */
    }

    /* --- Right valley floor: min between peak_idx and the nearest taller peak to the right --- */
    float right_valley = peak_val;
    for (uint16_t j = peak_idx + 1; j < n; j++) {
        if (I_uA[j] < right_valley) right_valley = I_uA[j];
        if (I_uA[j] >= peak_val)    break; /* hit a taller neighbour */
    }

    /* Prominence is measured from the HIGHER of the two valley floors. */
    float base = (left_valley > right_valley) ? left_valley : right_valley;
    return peak_val - base;
}

/* --------------------------------------------------------------------------
 * Internal: insertion-sort the out[] array by I_uA descending.
 * n is small (≤ max_peaks, typically ≤ 8) so O(n²) is fine.
 * -------------------------------------------------------------------------- */
static void sort_peaks_descending(peak_t *peaks, uint16_t count)
{
    for (uint16_t i = 1; i < count; i++) {
        peak_t key = peaks[i];
        int j = (int)i - 1;
        while (j >= 0 && peaks[j].I_uA < key.I_uA) {
            peaks[j + 1] = peaks[j];
            j--;
        }
        peaks[j + 1] = key;
    }
}

uint16_t peaks_find(const float *I_uA, const float *E_mV, uint16_t n,
                    peak_t *out, uint16_t max_peaks, float prominence_threshold)
{
    if (!I_uA || !E_mV || n < 3 || !out || max_peaks == 0) {
        return 0;
    }

    uint16_t found = 0;

    /* Pass 1: collect all local maxima with sufficient prominence. */
    for (uint16_t i = 1; i + 1 < n && found < max_peaks; i++) {
        /* Local maximum test: strictly greater than both immediate neighbours. */
        if (I_uA[i] <= I_uA[i - 1] || I_uA[i] <= I_uA[i + 1]) {
            continue;
        }

        float prom = compute_prominence(I_uA, n, i);
        if (prom < prominence_threshold) {
            continue;
        }

        out[found].E_mV  = E_mV[i];
        out[found].I_uA  = I_uA[i];
        out[found].index = i;
        found++;
    }

    /* Pass 2: sort results by I_uA descending so the tallest peak is first. */
    if (found > 1) {
        sort_peaks_descending(out, found);
    }

    return found;
}
