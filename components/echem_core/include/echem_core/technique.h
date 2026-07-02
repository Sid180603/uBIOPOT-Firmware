#pragma once

/**
 * @file technique.h
 * @brief Technique interface, registry, and HAL dependency-injection callbacks.
 *
 * The HAL callbacks (hal_callbacks_t) are the DI seam: the DPV algorithm calls ONLY
 * these function pointers and never touches hardware directly. This allows:
 *   - Firmware: bind to pstat_hal functions (P1).
 *   - Host tests: bind to mock functions returning synthetic data (P2).
 *   - Wokwi simulation: bind to simulated ADS1115/MCP4921 (test pyramid L7).
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file.
 */

#include "calibration.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Electrode selection
 * -------------------------------------------------------------------------- */

typedef enum {
    ELECTRODE_1   = 1,  /**< Select electrode 1 (T1, GPIO32). */
    ELECTRODE_2   = 2,  /**< Select electrode 2 (T2, GPIO25). */
    ELECTRODE_3   = 3,  /**< Select electrode 3 (T3, GPIO33). */
    ELECTRODE_ALL = 0,  /**< Sequential scan of all 3 electrodes (1→2→3). */
} electrode_t;

/* --------------------------------------------------------------------------
 * HAL dependency-injection callbacks
 * Filled by pstat_hal (firmware) or mock functions (host tests).
 * -------------------------------------------------------------------------- */

/**
 * @brief  Hardware abstraction callbacks injected into every technique run.
 *
 * The electrochemistry algorithm is ONLY allowed to call these. It never touches
 * SPI, I2C, or GPIO directly. This is the key design decision that separates the
 * algorithm from the hardware and enables host-unit-testing without a board.
 */
typedef struct {
    /**
     * Set the cell voltage (V) via the DAC.
     * Maps to pstat_dac_set_volt() on firmware.
     */
    void  (*set_voltage)(float cell_v_volts, void *ctx);

    /**
     * Read current (µA) as average of n_avg consecutive ADS1115 samples.
     * AIN1 stays in continuous mode — no channel switch overhead.
     * Maps to pstat_adc_read_current_uA() on firmware.
     */
    float (*read_current_uA)(uint8_t n_avg, void *ctx);

    /**
     * Read the cell voltage (V) from ADS1115 AIN0 (RE channel, on-demand).
     * Involves a channel switch + stale-sample discard inside the HAL.
     * Maps to pstat_adc_read_cell_volt() on firmware.
     */
    float (*read_cell_volt)(void *ctx);

    /**
     * Emit one complete data point to the acquisition engine fan-out.
     * @param E_mV    Step potential (mV) — the BASE potential (not E + E_pulse).
     * @param I_uA    Differential current dI = I_pulse − I_base (µA).
     * @param RE_mV   Measured RE voltage (mV) — for x-axis iR-lag correction in UI.
     * @param ctx     Opaque context.
     */
    void  (*emit_point)(float E_mV, float I_uA, float RE_mV, void *ctx);

    /**
     * Return true if an abort was requested (e.g. button long-press, WS abort command).
     * The engine sets an atomic flag; the algorithm polls this every step.
     */
    bool  (*check_abort)(void *ctx);

    /**
     * Blocking delay in milliseconds.
     * Maps to vTaskDelay(pdMS_TO_TICKS(ms)) on firmware, usleep() in host tests.
     */
    void  (*delay_ms)(uint32_t ms, void *ctx);

    /** Opaque context pointer passed to every callback above. */
    void *ctx;
} hal_callbacks_t;

/* --------------------------------------------------------------------------
 * Technique interface
 * -------------------------------------------------------------------------- */

/** Opaque parameter type — each technique casts to its own concrete params struct. */
typedef void technique_params_t;

/** Maximum number of simultaneously registered techniques. */
#define TECHNIQUE_REGISTRY_MAX 8

/**
 * @brief  Technique descriptor.
 *
 * To add a new technique: fill one of these and call technique_register().
 * In v1 only DPV is registered; CV/LSV/SWV/NPV are reserved stubs.
 */
typedef struct {
    const char *name;         /**< Short name used in JSON commands, e.g. "DPV". */
    size_t      params_size;  /**< sizeof the concrete params struct (e.g. sizeof(dpv_params_t)). */

    /** Populate *params with recommended defaults. params must point to params_size bytes. */
    void (*get_defaults)(technique_params_t *params);

    /**
     * Validate params. Returns 0 = valid, negative error code = invalid.
     * On error, if err_buf != NULL, writes a human-readable message.
     */
    int  (*validate)(const technique_params_t *params, char *err_buf, size_t err_len);

    /**
     * Run the technique.
     * @return 0 = scan complete, positive = aborted, negative = error.
     * Must poll hal->check_abort(ctx) at every step and return early if true.
     */
    int  (*run)(const technique_params_t *params,
                const pstat_calib_t     *cal,
                const hal_callbacks_t   *hal);
} technique_t;

/* --------------------------------------------------------------------------
 * Registry API
 * -------------------------------------------------------------------------- */

/**
 * @brief  Initialise the registry and register all compiled-in techniques.
 *         Must be called once at boot before any technique_find().
 *         In v1: registers DPV only. CV/LSV/SWV/NPV are NOT registered (stubs).
 */
void technique_registry_init(void);

/**
 * @brief  Register a technique. technique_registry_init() calls this internally.
 * @return 0 on success, -1 if the registry is full (TECHNIQUE_REGISTRY_MAX exceeded).
 */
int technique_register(const technique_t *t);

/**
 * @brief  Look up a technique by name (exact, case-sensitive match).
 * @return Pointer to the registered technique_t, or NULL if not found.
 *         Returns NULL for "CV", "LSV", "SWV", "NPV" until they are implemented.
 */
const technique_t *technique_find(const char *name);

#ifdef __cplusplus
}
#endif
