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
    /*
     * TODO P2: implement full DPV algorithm.
     *
     * Algorithm outline (to be filled in P2):
     *
     * 1. Equilibration: set_voltage(E_begin); delay_ms(t_equilibration_ms)
     * 2. For each cycle c in [0, cycles):
     *    a. Determine step direction: sign = (e_end > e_begin) ? +1 : -1
     *    b. For each step E from e_begin to e_end by sign * e_step_mV:
     *       i.  set_voltage(E)
     *       ii. delay_ms(t_period_ms - t_pulse_ms - avg_timing_overhead)
     *       iii. I_base = read_current_uA(n_avg)       ← END of baseline
     *       iv.  set_voltage(E + sign * e_pulse_mV)
     *       v.   delay_ms(t_pulse_ms - avg_timing_overhead)
     *       vi.  I_pulse = read_current_uA(n_avg)      ← END of pulse
     *       vii. RE_mV = read_cell_volt() * 1000.0f
     *       viii. emit_point(E, I_pulse - I_base, RE_mV)
     *       ix.  if (check_abort()) return +1;          ← instant abort, < 1 step latency
     *
     * Fixes vs original Python2.ino:
     *   - Stateless/re-entrant (no sticky `cycles` member)
     *   - Separate V/I calibration (no copy-paste polynomial bug)
     *   - Real timed averaging with overhead compensation
     *   - Measured RE voltage (not reconstructed from DAC count)
     *   - Abort support at every step
     *
     * This stub compiles cleanly and satisfies P0 DoD. P2 fills in the body.
     */
    (void)params;
    (void)cal;
    (void)hal;
    return 0; /* "complete" */
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
