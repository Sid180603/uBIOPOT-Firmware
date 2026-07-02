/**
 * test_calibration.c
 * Unity host tests for echem_core/calibration.c
 *
 * Tests the physical unit conversion functions independently of any hardware.
 * All expected values derived analytically from the OP07 board constants.
 * These tests run in CI on every commit — no board required.
 */

#include "unity.h"
#include "echem_core/calibration.h"
#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/* ---- helpers ---- */
static pstat_calib_t default_cal(void)
{
    pstat_calib_t c = PSTAT_CALIB_DEFAULT;
    return c;
}

/* ==============================================================================
 * calib_volt_to_dac / calib_dac_to_volt
 * ============================================================================== */

/* At cell_v = 0V, DAC code = round(vref_mid / dac_vref * 4095)
 * = round(1.995 / 3.3 * 4095) = round(2477.27) = 2477 */
void test_volt_to_dac_at_zero_cell_voltage(void)
{
    pstat_calib_t cal = default_cal();
    uint16_t code = calib_volt_to_dac(0.0f, &cal);
    TEST_ASSERT_INT_WITHIN(2, 2477, (int)code);
}

/* At cell_v = +1V (max anodic), code = round((1.995+1.0)/3.3*4095) = round(3718.2) = 3718 */
void test_volt_to_dac_at_plus_1V(void)
{
    pstat_calib_t cal = default_cal();
    uint16_t code = calib_volt_to_dac(1.0f, &cal);
    TEST_ASSERT_INT_WITHIN(3, 3718, (int)code);
}

/* At cell_v = -1V (max cathodic), code = round(0.995/3.3*4095) = round(1235.2) = 1235 */
void test_volt_to_dac_at_minus_1V(void)
{
    pstat_calib_t cal = default_cal();
    uint16_t code = calib_volt_to_dac(-1.0f, &cal);
    TEST_ASSERT_INT_WITHIN(3, 1235, (int)code);
}

/* Round-trip: convert voltage → code → voltage, expect ≤ 1 LSB error (~0.8 mV) */
void test_dac_roundtrip_zero(void)
{
    pstat_calib_t cal = default_cal();
    float original  = 0.0f;
    uint16_t code   = calib_volt_to_dac(original, &cal);
    float recovered = calib_dac_to_volt(code, &cal);
    /* 1 LSB = 3.3/4095 ≈ 0.806 mV */
    TEST_ASSERT_FLOAT_WITHIN(0.002f, original, recovered);
}

void test_dac_roundtrip_positive(void)
{
    pstat_calib_t cal = default_cal();
    float original  = 0.5f;
    uint16_t code   = calib_volt_to_dac(original, &cal);
    float recovered = calib_dac_to_volt(code, &cal);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, original, recovered);
}

/* Clamp: way above range → 4095 */
void test_dac_clamp_max(void)
{
    pstat_calib_t cal = default_cal();
    uint16_t code = calib_volt_to_dac(10.0f, &cal);
    TEST_ASSERT_EQUAL_UINT16(4095, code);
}

/* Clamp: way below range → 0 */
void test_dac_clamp_min(void)
{
    pstat_calib_t cal = default_cal();
    uint16_t code = calib_volt_to_dac(-10.0f, &cal);
    TEST_ASSERT_EQUAL_UINT16(0, code);
}

/* ==============================================================================
 * calib_adc_raw_to_volt
 * ============================================================================== */

/* 1 LSB @ GAIN_ONE = 125 µV = 0.000125 V */
void test_adc_raw_to_volt_one_lsb(void)
{
    pstat_calib_t cal = default_cal(); /* adc_lsb_uv = 125 */
    float v = calib_adc_raw_to_volt(1, &cal);
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 0.000125f, v);
}

void test_adc_raw_to_volt_zero(void)
{
    pstat_calib_t cal = default_cal();
    float v = calib_adc_raw_to_volt(0, &cal);
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 0.0f, v);
}

/* AIN1 at vref_mid (1.995 V) → raw = 1.995 / 0.000125 = 15960 */
void test_adc_raw_to_volt_at_vref_mid(void)
{
    pstat_calib_t cal = default_cal();
    int16_t raw = (int16_t)(1.995f / 0.000125f); /* = 15960 */
    float v = calib_adc_raw_to_volt(raw, &cal);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.995f, v);
}

/* ==============================================================================
 * calib_vout_to_current_uA
 * Prof-verified: Rf = 1 kΩ, formula = (vref_mid - vout) * 1000 µA
 * Anodic current: vout < vref_mid → I_uA > 0
 * ============================================================================== */

/* At vout = vref_mid → I = 0 µA */
void test_current_zero_at_vref_mid(void)
{
    pstat_calib_t cal = default_cal();
    float I = calib_vout_to_current_uA(cal.vref_mid, &cal);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, I);
}

/* +1 mA anodic: vout = vref_mid - 1.0 V (TIA drops by Rf*I = 1Ω*1mA = 1V)
 * Expected I = (1.995 - 0.995) * 1000 = 1000 µA */
void test_current_1mA_anodic(void)
{
    pstat_calib_t cal = default_cal();
    float vout = cal.vref_mid - 1.0f; /* 0.995 V */
    float I    = calib_vout_to_current_uA(vout, &cal);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 1000.0f, I);
}

/* -1 mA cathodic: vout = vref_mid + 1.0 V
 * Expected I = (1.995 - 2.995) * 1000 = -1000 µA */
void test_current_1mA_cathodic(void)
{
    pstat_calib_t cal = default_cal();
    float vout = cal.vref_mid + 1.0f; /* 2.995 V */
    float I    = calib_vout_to_current_uA(vout, &cal);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, -1000.0f, I);
}

/* Gain correction: current_gain=2.0 should double the result */
void test_current_gain_correction(void)
{
    pstat_calib_t cal = default_cal();
    cal.current_gain = 2.0f;
    float vout = cal.vref_mid - 0.5f; /* 500 µA raw */
    float I    = calib_vout_to_current_uA(vout, &cal);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 1000.0f, I); /* 500 * 2.0 = 1000 */
}

/* Offset correction: current_offset_uA=50 should shift result */
void test_current_offset_correction(void)
{
    pstat_calib_t cal = default_cal();
    cal.current_offset_uA = 50.0f;
    float I = calib_vout_to_current_uA(cal.vref_mid, &cal); /* raw=0, + offset */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 50.0f, I);
}

/* ==============================================================================
 * Entry point
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_volt_to_dac_at_zero_cell_voltage);
    RUN_TEST(test_volt_to_dac_at_plus_1V);
    RUN_TEST(test_volt_to_dac_at_minus_1V);
    RUN_TEST(test_dac_roundtrip_zero);
    RUN_TEST(test_dac_roundtrip_positive);
    RUN_TEST(test_dac_clamp_max);
    RUN_TEST(test_dac_clamp_min);
    RUN_TEST(test_adc_raw_to_volt_one_lsb);
    RUN_TEST(test_adc_raw_to_volt_zero);
    RUN_TEST(test_adc_raw_to_volt_at_vref_mid);
    RUN_TEST(test_current_zero_at_vref_mid);
    RUN_TEST(test_current_1mA_anodic);
    RUN_TEST(test_current_1mA_cathodic);
    RUN_TEST(test_current_gain_correction);
    RUN_TEST(test_current_offset_correction);
    return UNITY_END();
}
