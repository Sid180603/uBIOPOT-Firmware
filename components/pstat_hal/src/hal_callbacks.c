#include "pstat_hal/pstat_hal.h"
#include "echem_core/technique.h"
#include "echem_core/calibration.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Dependency-injection wiring: HAL functions → echem_core hal_callbacks_t.
 *
 * Each wrapper matches exactly one hal_callbacks_t function-pointer typedef.
 * The compiler enforces the signatures — a mismatch here IS the intended
 * compile-time check (P1 DoD: "HAL wired to echem_core typedefs").
 *
 * emit_point and check_abort are intentionally NULL here; they are wired
 * to the acquisition engine's queue/atomic-flag by acq_engine in P3.
 */

static void hal_set_voltage_fn(float cell_v, void *ctx)
{
    const pstat_calib_t *cal = (const pstat_calib_t *)ctx;
    pstat_dac_set_volt(cell_v, cal);
}

static float hal_read_current_fn(uint8_t n_avg, void *ctx)
{
    const pstat_calib_t *cal = (const pstat_calib_t *)ctx;
    return pstat_adc_read_current_uA(n_avg, cal);
}

static float hal_read_cell_volt_fn(void *ctx)
{
    const pstat_calib_t *cal = (const pstat_calib_t *)ctx;
    return pstat_adc_read_cell_volt(cal);
}

static void hal_delay_ms_fn(uint32_t ms, void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

hal_callbacks_t pstat_make_hal_callbacks(pstat_calib_t *cal)
{
    hal_callbacks_t cbs = {
        .set_voltage      = hal_set_voltage_fn,
        .read_current_uA  = hal_read_current_fn,
        .read_cell_volt   = hal_read_cell_volt_fn,
        .emit_point       = NULL,  /* wired by acq_engine (P3) */
        .check_abort      = NULL,  /* wired by acq_engine (P3) */
        .delay_ms         = hal_delay_ms_fn,
        .ctx              = cal,
    };
    return cbs;
}
