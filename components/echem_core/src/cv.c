#include "echem_core/cv.h"
#include "echem_core/calibration.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- Validation error codes ---- */
#define CV_ERR_E_RANGE    (-1)  /**< a vertex/begin out of ±1000 mV */
#define CV_ERR_STEP_ZERO  (-2)  /**< e_step_mV <= 0 */
#define CV_ERR_RATE_ZERO  (-3)  /**< scan_rate_mV_s <= 0 */
#define CV_ERR_CYCLES     (-4)  /**< cycles == 0 */
#define CV_ERR_ELECTRODE  (-5)  /**< electrode not 0/1/2/3 */
#define CV_ERR_N_AVG      (-6)  /**< n_avg == 0 */

/* ---- Technique interface implementations ---- */

static void cv_get_defaults(technique_params_t *params)
{
    cv_params_t def = CV_PARAMS_DEFAULT;
    memcpy(params, &def, sizeof(cv_params_t));
}

static int cv_validate(const technique_params_t *params, char *err, size_t elen)
{
    const cv_params_t *p = (const cv_params_t *)params;

    if (p->e_begin_mV   < -1000.0f || p->e_begin_mV   > 1000.0f ||
        p->e_vertex1_mV < -1000.0f || p->e_vertex1_mV > 1000.0f ||
        p->e_vertex2_mV < -1000.0f || p->e_vertex2_mV > 1000.0f) {
        if (err && elen) snprintf(err, elen, "E out of +/-1000 mV range");
        return CV_ERR_E_RANGE;
    }
    if (p->e_step_mV <= 0.0f) {
        if (err && elen) snprintf(err, elen, "e_step_mV must be > 0");
        return CV_ERR_STEP_ZERO;
    }
    if (p->scan_rate_mV_s <= 0.0f) {
        if (err && elen) snprintf(err, elen, "scan_rate_mV_s must be > 0");
        return CV_ERR_RATE_ZERO;
    }
    if (p->cycles == 0) {
        if (err && elen) snprintf(err, elen, "cycles must be >= 1");
        return CV_ERR_CYCLES;
    }
    if ((int)p->electrode < 0 || (int)p->electrode > 3) {
        if (err && elen) snprintf(err, elen, "electrode must be 0 (ALL), 1, 2, or 3");
        return CV_ERR_ELECTRODE;
    }
    if (p->n_avg == 0) {
        if (err && elen) snprintf(err, elen, "n_avg must be >= 1");
        return CV_ERR_N_AVG;
    }
    return 0;
}

/*
 * Ramp one linear segment from a_mV to b_mV in e_step_mV increments (excluding
 * the start point a, which is emitted by the previous segment / the initial
 * point).  Emits (E, I, RE) at each step.  Returns 1 if aborted, else 0.
 */
static int cv_segment(const cv_params_t *p, const hal_callbacks_t *hal,
                      float a_mV, float b_mV, uint32_t ms_per_step)
{
    const float mV_to_V = 1.0e-3f;
    const float span    = b_mV - a_mV;
    const float dir     = (span >= 0.0f) ? 1.0f : -1.0f;
    const int   steps   = (int)floorf(fabsf(span) / p->e_step_mV);

    for (int s = 1; s <= steps; s++) {
        float E = a_mV + dir * (float)s * p->e_step_mV;
        hal->set_voltage(E * mV_to_V, hal->ctx);
        if (ms_per_step > 0u) hal->delay_ms(ms_per_step, hal->ctx);

        float I  = hal->read_current_uA(p->n_avg, hal->ctx);
        float RE = hal->read_cell_volt(hal->ctx) * 1.0e3f;
        hal->emit_point(E, I, RE, hal->ctx);

        if (hal->check_abort(hal->ctx)) return 1;
    }
    return 0;
}

static int cv_run(const technique_params_t *params,
                  const pstat_calib_t     *cal,
                  const hal_callbacks_t   *hal)
{
    (void)cal;  /* HAL owns calibration internally for now. */
    const cv_params_t *p = (const cv_params_t *)params;
    const float mV_to_V  = 1.0e-3f;

    /* Time per staircase step so the average sweep rate matches scan_rate_mV_s. */
    uint32_t ms_per_step = (uint32_t)lroundf((p->e_step_mV / p->scan_rate_mV_s) * 1000.0f);

    /* Equilibrate at e_begin, broken into 50 ms slices for abort responsiveness. */
    hal->set_voltage(p->e_begin_mV * mV_to_V, hal->ctx);
    uint32_t eq_remaining = p->t_equilibration_ms;
    while (eq_remaining > 0u) {
        uint32_t slice = eq_remaining < 50u ? eq_remaining : 50u;
        hal->delay_ms(slice, hal->ctx);
        eq_remaining -= slice;
        if (hal->check_abort(hal->ctx)) return 1;  /* aborted during equilibration */
    }

    /* Emit the starting point at e_begin so the loop starts cleanly.  This is
     * also the first emit_point() call, which fires the EQUILIB_DONE event. */
    {
        float I  = hal->read_current_uA(p->n_avg, hal->ctx);
        float RE = hal->read_cell_volt(hal->ctx) * 1.0e3f;
        hal->emit_point(p->e_begin_mV, I, RE, hal->ctx);
        if (hal->check_abort(hal->ctx)) return 1;
    }

    for (uint8_t c = 0; c < p->cycles; c++) {
        if (cv_segment(p, hal, p->e_begin_mV,   p->e_vertex1_mV, ms_per_step)) return 1;
        if (cv_segment(p, hal, p->e_vertex1_mV, p->e_vertex2_mV, ms_per_step)) return 1;
        /* Close the loop back to e_begin if vertex2 differs (so the next cycle
         * starts from the same potential). */
        if (p->e_vertex2_mV != p->e_begin_mV) {
            if (cv_segment(p, hal, p->e_vertex2_mV, p->e_begin_mV, ms_per_step)) return 1;
        }
    }

    return 0;  /* scan complete */
}

static const technique_t s_cv_technique = {
    .name         = "CV",
    .params_size  = sizeof(cv_params_t),
    .get_defaults = cv_get_defaults,
    .validate     = cv_validate,
    .run          = cv_run,
};

const technique_t *cv_get_technique(void)
{
    return &s_cv_technique;
}
