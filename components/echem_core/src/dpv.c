#include "echem_core/dpv.h"
#include "echem_core/calibration.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- Validation error codes ---- */
#define DPV_ERR_E_RANGE      (-1)  /**< E_begin or E_end out of ±1000 mV */
#define DPV_ERR_STEP_ZERO    (-2)  /**< e_step_mV <= 0 */
#define DPV_ERR_PULSE_ZERO   (-3)  /**< e_pulse_mV <= 0 */
#define DPV_ERR_TIMING       (-4)  /**< t_pulse >= t_period or t_pulse == 0 */
#define DPV_ERR_CYCLES       (-5)  /**< cycles == 0 */
#define DPV_ERR_ELECTRODE    (-6)  /**< electrode not 0/1/2/3 */
#define DPV_ERR_N_AVG        (-7)  /**< n_avg == 0 */

/* ---- Technique interface implementations ---- */

static void dpv_get_defaults(technique_params_t *params)
{
    dpv_params_t def = DPV_PARAMS_DEFAULT;
    memcpy(params, &def, sizeof(dpv_params_t));
}

static int dpv_validate(const technique_params_t *params, char *err, size_t elen)
{
    const dpv_params_t *p = (const dpv_params_t *)params;

    if (p->e_begin_mV < -1000.0f || p->e_begin_mV > 1000.0f ||
        p->e_end_mV   < -1000.0f || p->e_end_mV   > 1000.0f) {
        if (err && elen) snprintf(err, elen, "E out of +/-1000 mV range");
        return DPV_ERR_E_RANGE;
    }
    if (p->e_step_mV <= 0.0f) {
        if (err && elen) snprintf(err, elen, "e_step_mV must be > 0");
        return DPV_ERR_STEP_ZERO;
    }
    if (p->e_pulse_mV <= 0.0f) {
        if (err && elen) snprintf(err, elen, "e_pulse_mV must be > 0");
        return DPV_ERR_PULSE_ZERO;
    }
    if (p->t_pulse_ms == 0 || p->t_pulse_ms >= p->t_period_ms) {
        if (err && elen) snprintf(err, elen, "t_pulse_ms must be > 0 and < t_period_ms");
        return DPV_ERR_TIMING;
    }
    if (p->cycles == 0) {
        if (err && elen) snprintf(err, elen, "cycles must be >= 1");
        return DPV_ERR_CYCLES;
    }
    if ((int)p->electrode < 0 || (int)p->electrode > 3) {
        if (err && elen) snprintf(err, elen, "electrode must be 0 (ALL), 1, 2, or 3");
        return DPV_ERR_ELECTRODE;
    }
    if (p->n_avg == 0) {
        if (err && elen) snprintf(err, elen, "n_avg must be >= 1");
        return DPV_ERR_N_AVG;
    }
    return 0;
}

static int dpv_run(const technique_params_t *params,
                   const pstat_calib_t     *cal,
                   const hal_callbacks_t   *hal)
{
    const dpv_params_t *p = (const dpv_params_t *)params;

    /* cal is available for future inline conversions (e.g. volt_to_dac inside the algo).
     * The HAL callbacks own calibration internally for now; suppress the warning. */
    (void)cal;

    /*
     * Timing overhead for one averaging call (n_avg samples @ ~860 SPS).
     * At 860 SPS the ADS1115 produces one sample every ~1.163 ms.
     * We space samples 1.2 ms apart to be safe.
     * Overhead per read = n_avg * 1.2 ms.
     *
     * This is subtracted from the delay periods so the true baseline and pulse
     * durations remain accurate regardless of n_avg.
     *
     * In host tests n_avg is typically 1 and the mock delay_ms is a no-op,
     * so overhead_ms = 0 in practice — the test sees the raw timing numbers.
     */
    const uint32_t sample_interval_ms = 2u; /* conservative; real HW uses 1.2 ms */
    const uint32_t overhead_ms = (uint32_t)p->n_avg * sample_interval_ms;

    /* Clamp so subtraction never underflows (guards against tiny t_pulse_ms). */
    uint32_t baseline_delay_ms = (p->t_period_ms > p->t_pulse_ms + overhead_ms)
                                 ? (p->t_period_ms - p->t_pulse_ms - overhead_ms)
                                 : 0u;
    uint32_t pulse_delay_ms    = (p->t_pulse_ms > overhead_ms)
                                 ? (p->t_pulse_ms - overhead_ms)
                                 : 0u;

    /* Scan direction: positive if sweeping anodic (e_end > e_begin). */
    float sign = (p->e_end_mV >= p->e_begin_mV) ? 1.0f : -1.0f;

    /* Convert mV → V for set_voltage callbacks. */
    const float mV_to_V = 1.0e-3f;

    /* ------------------------------------------------------------------ */
    /* Equilibration: hold at e_begin to let the electrode stabilise.      */
    /* ------------------------------------------------------------------ */
    hal->set_voltage(p->e_begin_mV * mV_to_V, hal->ctx);
    if (p->t_equilibration_ms > 0u) {
        hal->delay_ms(p->t_equilibration_ms, hal->ctx);
    }

    /* Check abort even before the sweep starts. */
    if (hal->check_abort(hal->ctx)) {
        return 1; /* aborted */
    }

    /* ------------------------------------------------------------------ */
    /* Sweep: cycles × steps.                                              */
    /* ------------------------------------------------------------------ */
    for (uint8_t cycle = 0; cycle < p->cycles; cycle++) {

        /* Number of steps: floor(span/e_step) + 1 so the endpoint is included
         * when span is exactly divisible by e_step (PalmSens/Autolab convention).
         * For non-exact spans both ceil and floor+1 give the same result. */
        float span  = (p->e_end_mV - p->e_begin_mV) * sign; /* always ≥ 0 */
        int   steps = (int)floorf(span / p->e_step_mV) + 1;
        if (steps <= 0) steps = 1; /* degenerate: at least one sample */

        for (int s = 0; s < steps; s++) {

            /* Compute E by index to avoid accumulated floating-point drift.
             * No last-step clamping: the natural step grid is correct. */
            float E = p->e_begin_mV + (float)s * sign * p->e_step_mV;

            /* ---- Baseline phase ---- */
            hal->set_voltage(E * mV_to_V, hal->ctx);
            if (baseline_delay_ms > 0u) {
                hal->delay_ms(baseline_delay_ms, hal->ctx);
            }
            float I_base = hal->read_current_uA(p->n_avg, hal->ctx);

            /* ---- Pulse phase ---- */
            float E_pulse = E + sign * p->e_pulse_mV;
            hal->set_voltage(E_pulse * mV_to_V, hal->ctx);
            if (pulse_delay_ms > 0u) {
                hal->delay_ms(pulse_delay_ms, hal->ctx);
            }
            float I_pulse = hal->read_current_uA(p->n_avg, hal->ctx);

            /* ---- RE readback (AIN0, on-demand) ---- */
            float RE_V  = hal->read_cell_volt(hal->ctx);
            float RE_mV = RE_V * 1.0e3f;

            /* ---- Emit data point ---- */
            /* x-axis = BASE potential E, y-axis = dI = I_pulse − I_base */
            hal->emit_point(E, I_pulse - I_base, RE_mV, hal->ctx);

            /* ---- Abort check (every step, < 1 step latency) ---- */
            if (hal->check_abort(hal->ctx)) {
                return 1; /* aborted */
            }
        }
    }

    return 0; /* scan complete */
}

static const technique_t s_dpv_technique = {
    .name         = "DPV",
    .params_size  = sizeof(dpv_params_t),
    .get_defaults = dpv_get_defaults,
    .validate     = dpv_validate,
    .run          = dpv_run,
};

const technique_t *dpv_get_technique(void)
{
    return &s_dpv_technique;
}
