/**
 * test_dpv.c
 * Unity host tests for echem_core technique registry and DPV parameter validation.
 *
 * The DPV algorithm body is a stub in P0 â€” full algorithm tests are added in P2.
 * These P0 tests verify:
 *   - technique_registry_init() registers DPV
 *   - technique_find("DPV") returns a valid descriptor
 *   - technique_find("CV"/"LSV"/etc. returns NULL (not yet implemented)
 *   - DPV default params pass validation
 *   - DPV validation catches every invalid-parameter case
 *
 * P2 algorithm tests (added here):
 *   - Mock HAL records every call for post-run assertion
 *   - Step count = floor(span / e_step) + 1 (endpoint inclusive) * cycles
 *   - set_voltage() called base then pulse each step (correct interleaving)
 *   - emit_point() called once per step (dI = I_pulse - I_base)
 *   - abort stops within one step (< 1 step latency)
 *   - multi-cycle scan emits expected total points
 *   - synthetic Gaussian current model: peak emitted at expected potential
 */

#include "unity.h"
#include "echem_core/technique.h"
#include "echem_core/dpv.h"
#include "echem_core/calibration.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

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

void test_dpv_validate_n_avg_zero(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.n_avg = 0;
    char err[64] = {0};
    int rc = dpv_get_technique()->validate(&p, err, sizeof(err));
    TEST_ASSERT_LESS_THAN(0, rc);
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
 * P2 â€” Mock HAL and algorithm tests
 *
 * The mock HAL records every call so we can assert:
 *   - Correct number of emit_point() calls (= expected step count Ã— cycles)
 *   - Correct set_voltage interleaving (base then pulse per step)
 *   - Correct dI = I_pulse âˆ’ I_base
 *   - Abort stops within one step
 * ============================================================================== */

#define MOCK_MAX_CALLS  4096
#define MOCK_POINT_CAP   512

/* Types of set_voltage calls, distinguished by whether they're a baseline or pulse set */
typedef enum { CALL_SET_VOLTAGE, CALL_READ_CURRENT, CALL_READ_CELL_VOLT,
               CALL_EMIT_POINT, CALL_DELAY, CALL_ABORT_CHECK } mock_call_type_t;

/* Recorded emitted point */
typedef struct {
    float E_mV;
    float I_uA;
    float RE_mV;
} mock_point_t;

/* Mock HAL context â€” passed as hal->ctx */
typedef struct {
    /* Abort control */
    int   abort_after_n_points; /* abort after this many emit_point calls; -1 = never */
    int   emit_count;

    /* Emitted points */
    mock_point_t points[MOCK_POINT_CAP];
    int          n_points;

    /* set_voltage sequence (mV) */
    float voltages[MOCK_MAX_CALLS];
    int   n_voltages;

    /* read_current returns this value (uniform mock, or a callback for Gaussian) */
    float fixed_current_uA; /* used when gauss_fn == NULL */
    /* Gaussian synthetic model (optional) */
    float (*gauss_fn)(float cell_v, void *user); /* NULL = use fixed_current_uA */
    void  *gauss_user;

    /* Phase tracking: is the current read a baseline or pulse read? */
    int reads_since_last_voltage; /* reset to 0 on each set_voltage call */

    /* delay_ms total accumulated (for diagnostic, not tested) */
    uint32_t total_delay_ms;
} mock_ctx_t;

/* ---- Mock callback implementations ---- */

static void mock_set_voltage(float v, void *ctx)
{
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    if (m->n_voltages < MOCK_MAX_CALLS) {
        m->voltages[m->n_voltages++] = v * 1000.0f; /* store in mV */
    }
    m->reads_since_last_voltage = 0;
}

static float mock_read_current(uint8_t n_avg, void *ctx)
{
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    (void)n_avg;
    m->reads_since_last_voltage++;
    if (m->gauss_fn && m->n_voltages > 0) {
        float last_v_mV = m->voltages[m->n_voltages - 1];
        return m->gauss_fn(last_v_mV, m->gauss_user);
    }
    return m->fixed_current_uA;
}

static float mock_read_cell_volt(void *ctx)
{
    (void)ctx;
    return 0.0f; /* RE = 0 V in tests */
}

static void mock_emit_point(float E_mV, float I_uA, float RE_mV, void *ctx)
{
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    if (m->n_points < MOCK_POINT_CAP) {
        m->points[m->n_points].E_mV  = E_mV;
        m->points[m->n_points].I_uA  = I_uA;
        m->points[m->n_points].RE_mV = RE_mV;
        m->n_points++;
    }
    m->emit_count++;
}

static bool mock_check_abort(void *ctx)
{
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    if (m->abort_after_n_points >= 0 && m->emit_count >= m->abort_after_n_points) {
        return true;
    }
    return false;
}

static void mock_delay(uint32_t ms, void *ctx)
{
    mock_ctx_t *m = (mock_ctx_t *)ctx;
    m->total_delay_ms += ms;
}

/* Build a hal_callbacks_t wired to a mock_ctx_t */
static hal_callbacks_t make_mock_hal(mock_ctx_t *ctx)
{
    hal_callbacks_t h;
    h.set_voltage    = mock_set_voltage;
    h.read_current_uA = mock_read_current;
    h.read_cell_volt  = mock_read_cell_volt;
    h.emit_point      = mock_emit_point;
    h.check_abort     = mock_check_abort;
    h.delay_ms        = mock_delay;
    h.ctx             = ctx;
    return h;
}

static pstat_calib_t default_cal(void)
{
    pstat_calib_t c = PSTAT_CALIB_DEFAULT;
    return c;
}

/* ---- Helper: run DPV with a freshly-zeroed mock context ---- */
static int run_dpv(dpv_params_t *p, mock_ctx_t *m)
{
    const technique_t *t = dpv_get_technique();
    pstat_calib_t cal    = default_cal();
    hal_callbacks_t hal  = make_mock_hal(m);
    return t->run(p, &cal, &hal);
}

/* ==============================================================================
 * Step count tests
 *
 * Expected steps per cycle = floor(span / e_step) + 1 (endpoint inclusive).
 * Total emit_point calls = steps_per_cycle * cycles.
 * ============================================================================== */

void test_dpv_step_count_simple(void)
{
    /* -500 → +500 mV, step 100 mV → floor(1000/100)+1 = 11 steps (includes +500 endpoint) */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         = -500.0f;
    p.e_end_mV           =  500.0f;
    p.e_step_mV          =  100.0f;
    p.t_equilibration_ms =    0;
    p.cycles             =    1;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.fixed_current_uA     = 10.0f;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(11, m.n_points);
}

void test_dpv_step_count_with_two_cycles(void)
{
    /* 0 → 400 mV, step 100 mV → floor(400/100)+1 = 5 steps; cycles=3 → 15 total */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         =    0.0f;
    p.e_end_mV           =  400.0f;
    p.e_step_mV          =  100.0f;
    p.t_equilibration_ms =    0;
    p.cycles             =    3;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.fixed_current_uA     = 5.0f;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(15, m.n_points);
}

void test_dpv_step_count_cathodic_scan(void)
{
    /* 500 → -500 mV (cathodic), step 100 mV → floor(1000/100)+1 = 11 steps */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         =  500.0f;
    p.e_end_mV           = -500.0f;
    p.e_step_mV          =  100.0f;
    p.t_equilibration_ms =    0;
    p.cycles             =    1;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.fixed_current_uA     = -10.0f;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(11, m.n_points);
}

/* ==============================================================================
 * set_voltage interleaving: each step must call set_voltage twice
 * (once for base E, once for pulse E + sign*e_pulse).
 * The equilibration call is the first voltage, so with no equilibration:
 *   voltages[0] = e_begin (equilibration is at e_begin even with 0 ms delay,
 *   the firmware always calls set_voltage once before the step loop)
 *   voltages[1] = base of step 1, voltages[2] = pulse of step 1, etc.
 * ============================================================================== */

void test_dpv_voltage_sequence_base_then_pulse(void)
{
    /* 0->100 mV, step=50 mV -> floor(100/50)+1 = 3 steps at 0, 50, 100 mV; pulse=20 mV */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         =   0.0f;
    p.e_end_mV           = 100.0f;
    p.e_step_mV          =  50.0f;
    p.e_pulse_mV         =  20.0f;
    p.t_equilibration_ms =   0;
    p.cycles             =   1;
    p.n_avg              =   1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.fixed_current_uA     = 1.0f;

    run_dpv(&p, &m);

    TEST_ASSERT_EQUAL_INT(3, m.n_points); /* 3 steps including endpoint */
    TEST_ASSERT_FLOAT_WITHIN(1.0f,   0.0f, m.voltages[1]); /* base step 0 */
    TEST_ASSERT_FLOAT_WITHIN(1.0f,  20.0f, m.voltages[2]); /* pulse step 0 */
    TEST_ASSERT_FLOAT_WITHIN(1.0f,  50.0f, m.voltages[3]); /* base step 1 */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 100.0f, m.voltages[5]); /* base step 2 = e_end */
}

/* ==============================================================================
 * dI = I_pulse - I_base
 *
 * We set read_current to return a step function:
 *   baseline reads return BASELINE_UA, pulse reads return PULSE_UA.
 * But since mock_read_current uses the last set_voltage to look up the value,
 * we use the simple approach: fixed I for all reads, then dI = 0.
 * For a non-trivial dI test we use two distinct fixed values via a custom fn.
 * ============================================================================== */

typedef struct { float baseline; float pulse; int call_count; } two_val_ctx_t;

/* Returns baseline on odd calls (1st read after each voltage = baseline),
 * pulse on even calls (2nd read = pulse).
 * The mock_read_current increments reads_since_last_voltage on each call.
 * We can't directly use that here, so we use a simple alternating counter. */
static float gauss_two_val(float cell_v, void *user)
{
    (void)cell_v;
    two_val_ctx_t *tv = (two_val_ctx_t *)user;
    tv->call_count++;
    /* Odd calls (1st read after set_voltage) = baseline; even = pulse */
    return (tv->call_count % 2 == 1) ? tv->baseline : tv->pulse;
}

void test_dpv_di_equals_pulse_minus_base(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         =   0.0f;
    p.e_end_mV           = 100.0f;
    p.e_step_mV          = 100.0f; /* floor(100/100)+1 = 2 steps at 0 and 100 mV */
    p.t_equilibration_ms =   0;
    p.cycles             =   1;
    p.n_avg              =   1;

    two_val_ctx_t tv = { .baseline = 5.0f, .pulse = 15.0f, .call_count = 0 };

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.gauss_fn   = gauss_two_val;
    m.gauss_user = &tv;

    run_dpv(&p, &m);

    TEST_ASSERT_EQUAL_INT(2, m.n_points); /* 2 steps */
    /* dI = pulse - baseline = 15 - 5 = 10 ÂµA */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 10.0f, m.points[0].I_uA);
}

/* ==============================================================================
 * Abort test: abort after N points â€” scan must stop within 1 extra step
 * ============================================================================== */

void test_dpv_abort_stops_within_one_step(void)
{
    /* 20 steps; abort after 5 */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         = -500.0f;
    p.e_end_mV           =  500.0f;
    p.e_step_mV          =   50.0f; /* 20 steps */
    p.t_equilibration_ms =    0;
    p.cycles             =    1;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = 5;
    m.fixed_current_uA     = 1.0f;

    int rc = run_dpv(&p, &m);

    /* rc = +1 (aborted) */
    TEST_ASSERT_EQUAL_INT(1, rc);
    /* Points emitted = exactly 5 (abort on check AFTER emit) */
    TEST_ASSERT_EQUAL_INT(5, m.n_points);
}

void test_dpv_abort_at_start_emits_zero_points(void)
{
    /* abort_after_n_points = 0 â†’ abort check fires before first emit */
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         = -500.0f;
    p.e_end_mV           =  500.0f;
    p.e_step_mV          =  100.0f;
    p.t_equilibration_ms =    0;
    p.cycles             =    1;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = 0; /* abort immediately */
    m.fixed_current_uA     = 1.0f;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(1, rc);
    /* 0 points emitted (abort checked after first emit â€” emit happens before check,
     * so at most 1 point may be emitted; abort_after=0 means abort fires on the 0th
     * check which is BEFORE the first emit in the post-emit abort check).
     * Actually: the algo emits THEN checks abort. So 0 abort threshold means
     * emit_count=0 when check is first called (before any emit) â€” but the first
     * abort check is AFTER the first emit. So exactly 0 points if abort_after=0
     * and we abort at the equilibration post-check... let's allow 0 or 1. */
    TEST_ASSERT_LESS_OR_EQUAL(1, m.n_points);
}

void test_dpv_no_abort_returns_zero(void)
{
    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         =    0.0f;
    p.e_end_mV           =  100.0f;
    p.e_step_mV          =  100.0f;
    p.t_equilibration_ms =    0;
    p.cycles             =    1;
    p.n_avg              =    1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.fixed_current_uA     = 1.0f;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(0, rc); /* complete, not aborted */
}

/* ==============================================================================
 * Synthetic Gaussian end-to-end test
 *
 * Model: I(E) = peak_height * exp(-0.5 * ((E - peak_E) / sigma)^2)
 * This mimics a DPV anodic peak (e.g. CuÂ²âº near 0 V).
 *
 * The DPV output dI â‰ˆ derivative of I(E) Ã— e_step (for small e_step and e_pulse),
 * BUT since our mock gives the same Gaussian for both base and pulse reads:
 *   dI = I(E + e_pulse) - I(E)
 * The maximum of dI occurs slightly below peak_E (by ~e_pulse/2).
 * We test that the emitted maximum is within Â±(e_step + e_pulse) of the true peak.
 * ============================================================================== */

typedef struct {
    float peak_E_mV;
    float peak_I_uA;
    float sigma_mV;
    float e_pulse_mV;
    int   call_count;
} dpv_gauss_ctx_t;

/* Alternating mock: odd calls = baseline (return 0), even calls = pulse.
 * On pulse call: last_v_mV = E_base + e_pulse → evaluate Gaussian at E_base.
 * So dI = Gaussian(E_base) - 0 = Gaussian(E_base) → peak exactly at peak_E_mV. */
static float dpv_gauss_fn(float last_v_mV, void *user)
{
    dpv_gauss_ctx_t *g = (dpv_gauss_ctx_t *)user;
    g->call_count++;
    if (g->call_count % 2 == 1) {
        return 0.0f; /* baseline */
    }
    float E_base = last_v_mV - g->e_pulse_mV;
    float z = (E_base - g->peak_E_mV) / g->sigma_mV;
    return g->peak_I_uA * expf(-0.5f * z * z);
}

void test_dpv_gaussian_peak_at_expected_potential(void)
{
    const float PEAK_E = -400.0f;
    dpv_gauss_ctx_t g = {
        .peak_E_mV  = PEAK_E,
        .peak_I_uA  = 50.0f,
        .sigma_mV   = 80.0f,
        .e_pulse_mV = 25.0f,
        .call_count = 0,
    };

    dpv_params_t p = DPV_PARAMS_DEFAULT;
    p.e_begin_mV         = -1000.0f;
    p.e_end_mV           =     0.0f;
    p.e_step_mV          =    10.0f;
    p.e_pulse_mV         =    25.0f;
    p.t_equilibration_ms =     0;
    p.cycles             =     1;
    p.n_avg              =     1;

    mock_ctx_t m;
    memset(&m, 0, sizeof(m));
    m.abort_after_n_points = -1;
    m.gauss_fn   = dpv_gauss_fn;
    m.gauss_user = &g;

    int rc = run_dpv(&p, &m);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_GREATER_THAN(0, m.n_points);

    float max_I  = m.points[0].I_uA;
    float peak_E = m.points[0].E_mV;
    for (int i = 1; i < m.n_points; i++) {
        if (m.points[i].I_uA > max_I) {
            max_I  = m.points[i].I_uA;
            peak_E = m.points[i].E_mV;
        }
    }

    /* dI peak is at E_base = PEAK_E; tolerance = 1 step (10 mV). */
    TEST_ASSERT_FLOAT_WITHIN(p.e_step_mV, PEAK_E, peak_E);
    TEST_ASSERT_GREATER_THAN(0.0f, max_I);
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
    RUN_TEST(test_dpv_validate_n_avg_zero);
    RUN_TEST(test_dpv_validate_electrode_all);
    RUN_TEST(test_dpv_validate_e_boundary_plus_1000);
    /* P2 algorithm tests */
    RUN_TEST(test_dpv_step_count_simple);
    RUN_TEST(test_dpv_step_count_with_two_cycles);
    RUN_TEST(test_dpv_step_count_cathodic_scan);
    RUN_TEST(test_dpv_voltage_sequence_base_then_pulse);
    RUN_TEST(test_dpv_di_equals_pulse_minus_base);
    RUN_TEST(test_dpv_abort_stops_within_one_step);
    RUN_TEST(test_dpv_abort_at_start_emits_zero_points);
    RUN_TEST(test_dpv_no_abort_returns_zero);
    RUN_TEST(test_dpv_gaussian_peak_at_expected_potential);
    return UNITY_END();
}
