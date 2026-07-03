/**
 * @file metal_id.c
 * @brief Metal identification and WHO limit lookup for DPV peak interpretation.
 *
 * Peak potential windows are typical values in 0.1 M acetate buffer pH 4.5
 * on a carbon or bismuth-film electrode vs Ag/AgCl reference.
 * Reference: WHO Guidelines for Drinking-water Quality 4th ed.
 *
 * PURE C — no esp_* or FreeRTOS headers.
 */

#include "echem_core/metal_id.h"
#include <math.h>

/* -------------------------------------------------------------------------
 * Static profile table
 *
 * Entries are ordered by peak potential (most negative first).
 * metal_identify() returns the first matching window, so the ordering
 * also defines preference when windows are adjacent or overlapping.
 *
 * WHO limits (µg/L) from WHO GDWQ 4th ed.:
 *   Zn  — no numeric guideline (aesthetic threshold 3000 µg/L)
 *   Cd  — 3 µg/L
 *   Pb  — 10 µg/L
 *   Cu  — 2000 µg/L
 *   As  — 10 µg/L
 *   Hg  — 6 µg/L
 * ------------------------------------------------------------------------- */

static const metal_profile_t k_profiles[] = {
    /*  sym   name        e_min_mV  e_max_mV  who_ugL  slope  intercept  calib */
    { "Zn", "Zinc",      -1100.0f,  -900.0f,    0.0f,  0.0f,    0.0f,  false },
    { "Cd", "Cadmium",    -800.0f,  -600.0f,    3.0f,  0.0f,    0.0f,  false },
    { "Pb", "Lead",       -550.0f,  -400.0f,   10.0f,  0.0f,    0.0f,  false },
    { "Cu", "Copper",     -100.0f,   +50.0f, 2000.0f,  0.0f,    0.0f,  false },
    { "As", "Arsenic",     +50.0f,  +200.0f,   10.0f,  0.0f,    0.0f,  false },
    { "Hg", "Mercury",    +200.0f,  +400.0f,    6.0f,  0.0f,    0.0f,  false },
};

#define N_PROFILES ((uint8_t)(sizeof(k_profiles) / sizeof(k_profiles[0])))

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

const metal_profile_t *metal_get_profiles(uint8_t *count_out)
{
    if (count_out) *count_out = N_PROFILES;
    return k_profiles;
}

const metal_profile_t *metal_identify(float E_mV)
{
    for (uint8_t i = 0; i < N_PROFILES; i++) {
        if (E_mV >= k_profiles[i].e_min_mV && E_mV <= k_profiles[i].e_max_mV) {
            return &k_profiles[i];
        }
    }
    return NULL; /* unknown peak potential */
}

float metal_concentration_uM(const metal_profile_t *profile, float I_peak_uA)
{
    if (!profile || !profile->calibrated || profile->slope_uA_per_uM == 0.0f) {
        return (float)NAN;
    }
    float c = (I_peak_uA - profile->intercept_uA) / profile->slope_uA_per_uM;
    return (c < 0.0f) ? 0.0f : c;
}
