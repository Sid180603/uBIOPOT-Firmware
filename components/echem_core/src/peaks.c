#include "echem_core/peaks.h"
#include <float.h>

uint16_t peaks_find(const float *I_uA, const float *E_mV, uint16_t n,
                    peak_t *out, uint16_t max_peaks, float prominence_threshold)
{
    /*
     * TODO P2: implement prominence-filtered local-maximum finder.
     *
     * Algorithm outline (to be filled in P2):
     *   1. Find all local maxima (I_uA[i] > I_uA[i-1] && I_uA[i] > I_uA[i+1]).
     *   2. For each candidate, compute its prominence:
     *        prominence = I_uA[peak] - max(valley_floor_to_left, valley_floor_to_right)
     *        where valley_floor = min(I_uA) in the region between this peak and any
     *        taller neighbouring peak (or the array boundary).
     *   3. Keep only peaks where prominence >= prominence_threshold.
     *   4. Sort by I_uA descending; write up to max_peaks into out[].
     *
     * This stub returns 0 peaks (compiles, satisfies P0 DoD).
     * P2 replaces this with the full implementation and host unit tests.
     */
    (void)I_uA;
    (void)E_mV;
    (void)n;
    (void)out;
    (void)max_peaks;
    (void)prominence_threshold;
    return 0;
}
