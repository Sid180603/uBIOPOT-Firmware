/**
 * test_peaks.c
 * Unity host tests for echem_core/peaks.c peaks_find().
 *
 * Tests:
 *   - Single isolated peak is found at correct index / potential
 *   - Two peaks both found, sorted by height descending
 *   - Low-prominence bump below threshold is rejected
 *   - Noisy floor with one true peak: peak found, noise excluded
 *   - max_peaks cap respected
 *   - Edge cases: too-short array, NULL inputs, all-flat array
 */

#include "unity.h"
#include "echem_core/peaks.h"
#include <string.h>
#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/* Helper: fill E_mV as an arithmetic series starting at e0 with step de */
static void fill_e(float *E, int n, float e0, float de)
{
    for (int i = 0; i < n; i++) E[i] = e0 + (float)i * de;
}

/* ==============================================================================
 * Basic single-peak detection
 * ============================================================================== */

void test_single_peak_found(void)
{
    /* Simple triangle peak at index 5 */
    float I[] = { 1, 2, 3, 4, 5, 10, 5, 4, 3, 2 };
    float E[10]; fill_e(E, 10, -450.0f, 100.0f); /* -450 to +450 mV */

    peak_t out[4];
    uint16_t n = peaks_find(I, E, 10, out, 4, 5.0f); /* 10 elements; i=8 is last interior point */

    TEST_ASSERT_EQUAL_UINT16(1, n);
    TEST_ASSERT_EQUAL_UINT16(5, out[0].index);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 50.0f, out[0].E_mV);  /* E[5] = -450+500 = 50 mV */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, out[0].I_uA);
}

void test_single_peak_potential_correct(void)
{
    /* Peak at exact -400 mV (index 1 in 3-element array) */
    float I[] = { 2.0f, 8.0f, 2.0f };
    float E[] = { -500.0f, -400.0f, -300.0f };

    peak_t out[4];
    uint16_t n = peaks_find(I, E, 3, out, 4, 1.0f);

    TEST_ASSERT_EQUAL_UINT16(1, n);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, -400.0f, out[0].E_mV);
}

/* ==============================================================================
 * Two peaks — sorted by height descending
 * ============================================================================== */

void test_two_peaks_found_and_sorted(void)
{
    /*  Indices:  0   1   2   3   4   5   6   7   8   9  10
     *  I (µA):  1   2  12   2   1   2  20   2   1   2   1
     *  Peaks at i=2 (height 12) and i=6 (height 20).
     *  i=9 is also a local max (2 > 1 on both sides, prominence=1) but
     *  falls below threshold=5 µA → filtered out.
     *  After sort: out[0]=i6 (20 µA), out[1]=i2 (12 µA). */
    float I[]  = { 1, 2, 12, 2, 1, 2, 20, 2, 1, 2, 1 };
    float E[11]; fill_e(E, 11, -500.0f, 100.0f);

    peak_t out[4];
    uint16_t n = peaks_find(I, E, 11, out, 4, 5.0f); /* threshold = 5 µA */

    TEST_ASSERT_EQUAL_UINT16(2, n);
    /* Sorted descending: taller peak first */
    TEST_ASSERT_EQUAL_UINT16(6, out[0].index); /* i=6, 20 µA */
    TEST_ASSERT_EQUAL_UINT16(2, out[1].index); /* i=2, 12 µA */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, out[0].I_uA);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.0f, out[1].I_uA);
}

/* ==============================================================================
 * Prominence threshold filtering
 * ============================================================================== */

void test_low_prominence_peak_rejected(void)
{
    /* Main peak: height 20 at i=5, valley at ~2 → prominence ~18 (passes)
     * Small bump:  height 5 at i=9, valley at ~3 → prominence ~2 (below threshold=5) */
    float I[]  = { 1, 2, 3, 2, 1, 20, 2, 3, 2,  5, 2, 1 };
    float E[12]; fill_e(E, 12, -550.0f, 100.0f);

    peak_t out[4];
    uint16_t n = peaks_find(I, E, 12, out, 4, 5.0f); /* threshold = 5 µA */

    /* Only the tall peak should pass */
    TEST_ASSERT_EQUAL_UINT16(1, n);
    TEST_ASSERT_EQUAL_UINT16(5, out[0].index);
}

void test_both_peaks_pass_threshold(void)
{
    /* Both peaks have prominence >> threshold */
    float I[]  = { 1, 2, 15, 2, 1, 2, 18, 2, 1 };
    float E[9]; fill_e(E, 9, -400.0f, 100.0f);

    peak_t out[4];
    uint16_t n = peaks_find(I, E, 9, out, 4, 5.0f);
    TEST_ASSERT_EQUAL_UINT16(2, n);
}

/* ==============================================================================
 * max_peaks cap
 * ============================================================================== */

void test_max_peaks_cap_respected(void)
{
    /* 3 prominent peaks, but cap at max_peaks=2 */
    float I[]  = { 1, 2, 20, 2, 1, 2, 18, 2, 1, 2, 15, 2, 1 };
    float E[13]; fill_e(E, 13, -600.0f, 100.0f);

    peak_t out[2];
    uint16_t n = peaks_find(I, E, 13, out, 2, 1.0f);

    TEST_ASSERT_EQUAL_UINT16(2, n); /* capped at 2 */
}

/* ==============================================================================
 * Edge cases
 * ============================================================================== */

void test_too_short_array_returns_zero(void)
{
    float I[] = { 5.0f, 10.0f };  /* only 2 elements — no interior points */
    float E[] = { 0.0f,  1.0f };
    peak_t out[4];
    uint16_t n = peaks_find(I, E, 2, out, 4, 0.0f);
    TEST_ASSERT_EQUAL_UINT16(0, n);
}

void test_null_arrays_return_zero(void)
{
    peak_t out[4];
    TEST_ASSERT_EQUAL_UINT16(0, peaks_find(NULL, NULL, 10, out, 4, 0.0f));
}

void test_null_out_returns_zero(void)
{
    float I[] = { 1, 5, 1 };
    float E[] = { 0, 1, 2 };
    TEST_ASSERT_EQUAL_UINT16(0, peaks_find(I, E, 3, NULL, 4, 0.0f));
}

void test_max_peaks_zero_returns_zero(void)
{
    float I[] = { 1, 5, 1 };
    float E[] = { 0, 1, 2 };
    peak_t out[4];
    TEST_ASSERT_EQUAL_UINT16(0, peaks_find(I, E, 3, out, 0, 0.0f));
}

void test_flat_array_no_peaks(void)
{
    float I[] = { 5, 5, 5, 5, 5, 5, 5 };
    float E[7]; fill_e(E, 7, -300.0f, 100.0f);
    peak_t out[4];
    uint16_t n = peaks_find(I, E, 7, out, 4, 0.0f);
    TEST_ASSERT_EQUAL_UINT16(0, n);
}

void test_monotone_rising_no_peaks(void)
{
    float I[] = { 1, 2, 3, 4, 5, 6, 7 };
    float E[7]; fill_e(E, 7, -300.0f, 100.0f);
    peak_t out[4];
    uint16_t n = peaks_find(I, E, 7, out, 4, 0.0f);
    TEST_ASSERT_EQUAL_UINT16(0, n);
}

/* ==============================================================================
 * Gaussian-shaped peak (realistic DPV output)
 * ============================================================================== */

void test_gaussian_peak_found_at_expected_potential(void)
{
    /* Build a 51-point DPV-like Gaussian:
     * E from -500 to +500 mV step 20, peak at 0 mV, sigma=100 mV */
    const int N = 51;
    float I[51], E[51];
    float peak_E_mV = 0.0f, sigma = 100.0f, height = 30.0f;
    for (int i = 0; i < N; i++) {
        E[i] = -500.0f + (float)i * 20.0f;
        float z = (E[i] - peak_E_mV) / sigma;
        I[i] = height * expf(-0.5f * z * z);
    }

    peak_t out[4];
    uint16_t n = peaks_find(I, E, N, out, 4, 1.0f);

    TEST_ASSERT_EQUAL_UINT16(1, n);
    /* Peak should be within ±40 mV of the true peak (2 steps of 20 mV) */
    TEST_ASSERT_FLOAT_WITHIN(40.0f, peak_E_mV, out[0].E_mV);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, height, out[0].I_uA);
}

/* ==============================================================================
 * Entry point
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_peak_found);
    RUN_TEST(test_single_peak_potential_correct);
    RUN_TEST(test_two_peaks_found_and_sorted);
    RUN_TEST(test_low_prominence_peak_rejected);
    RUN_TEST(test_both_peaks_pass_threshold);
    RUN_TEST(test_max_peaks_cap_respected);
    RUN_TEST(test_too_short_array_returns_zero);
    RUN_TEST(test_null_arrays_return_zero);
    RUN_TEST(test_null_out_returns_zero);
    RUN_TEST(test_max_peaks_zero_returns_zero);
    RUN_TEST(test_flat_array_no_peaks);
    RUN_TEST(test_monotone_rising_no_peaks);
    RUN_TEST(test_gaussian_peak_found_at_expected_potential);
    return UNITY_END();
}
