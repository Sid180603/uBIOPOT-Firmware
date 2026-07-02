/**
 * test_dpv.c
 * Unity host tests for echem_core technique registry and DPV parameter validation.
 *
 * The DPV algorithm body is a stub in P0 — full algorithm tests are added in P2.
 * These P0 tests verify:
 *   - technique_registry_init() registers DPV
 *   - technique_find("DPV") returns a valid descriptor
 *   - technique_find("CV"/"LSV"/etc. returns NULL (not yet implemented)
 *   - DPV default params pass validation
 *   - DPV validation catches every invalid-parameter case
 */

#include "unity.h"
#include "echem_core/technique.h"
#include "echem_core/dpv.h"

void setUp(void)    { technique_registry_init(); }
void tearDown(void) {}

/* ==============================================================================
 * Registry
 * ============================================================================== */

void test_registry_find_dpv_not_null(void)
{
    const technique_t *t = technique_find("DPV");
    TEST_ASSERT_NOT_NULL(t);
}

void test_registry_dpv_name_correct(void)
{
    const technique_t *t = technique_find("DPV");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("DPV", t->name);
}

void test_registry_dpv_params_size_correct(void)
{
    const technique_t *t = technique_find("DPV");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_size_t(sizeof(dpv_params_t), t->params_size);
}

/* Unimplemented techniques must return NULL, not crash */
void test_registry_cv_returns_null(void)
{
    TEST_ASSERT_NULL(technique_find("CV"));
}

void test_registry_lsv_returns_null(void)
{
    TEST_ASSERT_NULL(technique_find("LSV"));
}

void test_registry_swv_returns_null(void)
{
    TEST_ASSERT_NULL(technique_find("SWV"));
}

void test_registry_unknown_returns_null(void)
{
    TEST_ASSERT_NULL(technique_find("XYZZY"));
}

void test_registry_null_name_returns_null(void)
{
    TEST_ASSERT_NULL(technique_find(NULL));
}

/* ==============================================================================
 * DPV default params pass validation
 * ============================================================================== */

void test_dpv_defaults_pass_validation(void)
{
    const technique_t *t = technique_find("DPV");
    TEST_ASSERT_NOT_NULL(t);
    dpv_params_t p;
    t->get_defaults(&p);
    char err[128] = {0};
    int rc = t->validate(&p, err, sizeof(err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, err);
}

/* ==============================================================================
 * DPV validation error cases
 * ============================================================================== */

void test_dpv_validate_e_begin_too_low(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV = -1001.0f;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_e_end_too_high(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_end_mV = 1001.0f;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_step_zero(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_step_mV = 0.0f;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_step_negative(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_step_mV = -5.0f;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_pulse_zero(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_pulse_mV = 0.0f;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_pulse_equals_period(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.t_pulse_ms  = 200;
    p.t_period_ms = 200;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_pulse_exceeds_period(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.t_pulse_ms  = 300;
    p.t_period_ms = 200;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_cycles_zero(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.cycles = 0;
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

void test_dpv_validate_invalid_electrode(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.electrode = (electrode_t)5; /* invalid */
    TEST_ASSERT_LESS_THAN(0, dpv_get_technique()->validate(&p, NULL, 0));
}

/* ==============================================================================
 * DPV validation valid boundary values
 * ============================================================================== */

void test_dpv_validate_electrode_all(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.electrode = ELECTRODE_ALL;
    char err[64] = {0};
    int rc = dpv_get_technique()->validate(&p, err, sizeof(err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, err);
}

void test_dpv_validate_e_boundary_plus_1000(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV = -1000.0f;
    p.e_end_mV   =  1000.0f;
    char err[64] = {0};
    int rc = dpv_get_technique()->validate(&p, err, sizeof(err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, err);
}

/* ==============================================================================
 * Entry point
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registry_find_dpv_not_null);
    RUN_TEST(test_registry_dpv_name_correct);
    RUN_TEST(test_registry_dpv_params_size_correct);
    RUN_TEST(test_registry_cv_returns_null);
    RUN_TEST(test_registry_lsv_returns_null);
    RUN_TEST(test_registry_swv_returns_null);
    RUN_TEST(test_registry_unknown_returns_null);
    RUN_TEST(test_registry_null_name_returns_null);
    RUN_TEST(test_dpv_defaults_pass_validation);
    RUN_TEST(test_dpv_validate_e_begin_too_low);
    RUN_TEST(test_dpv_validate_e_end_too_high);
    RUN_TEST(test_dpv_validate_step_zero);
    RUN_TEST(test_dpv_validate_step_negative);
    RUN_TEST(test_dpv_validate_pulse_zero);
    RUN_TEST(test_dpv_validate_pulse_equals_period);
    RUN_TEST(test_dpv_validate_pulse_exceeds_period);
    RUN_TEST(test_dpv_validate_cycles_zero);
    RUN_TEST(test_dpv_validate_invalid_electrode);
    RUN_TEST(test_dpv_validate_electrode_all);
    RUN_TEST(test_dpv_validate_e_boundary_plus_1000);
    return UNITY_END();
}
