#pragma once

/**
 * @file pstat_hal.h
 * @brief Unified HAL contract consumed by acq_engine (P3) and bound to echem_core callbacks.
 *
 * This header IS allowed to include esp_err.h (pstat_hal is an IDF component).
 * It MUST NOT be included in echem_core sources (those must remain IDF-free).
 *
 * Only ONE backend is compiled for v1: MCP4921 (DAC) + ADS1115 (ADC).
 * The internal ESP32 DAC/ADC path was dropped (architecturally dead on the OP07 board —
 * GPIO25=T2 mux, GPIO26=MCP4921 CS; GPIO36/39 not routed to VOUT_UC/IOUT_UC).
 *
 * The HAL interface is kept abstract so a future board revision can supply a new backend
 * without changing echem_core, acq_engine, or any UI code.
 */

#include "esp_err.h"
#include "echem_core/calibration.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * DAC — MCP4921 on SPI3_HOST (VSPI)
 * Pins: CS=26, SCK=18, MOSI=23.
 * SPI word: 0x3000 | (code & 0x0FFF)  [BUF=0, GA=1x, SHDN=active]
 * ========================================================================== */

/** Initialise the MCP4921 SPI bus and device. Call once at boot. */
esp_err_t pstat_dac_init(void);

/** Output a raw 12-bit code [0, 4095]. Thread-safe (SPI transaction protected). */
esp_err_t pstat_dac_set_code(uint16_t code);

/**
 * @brief  Set cell voltage (V) via DAC, applying calibration from cal.
 *         Calls calib_volt_to_dac() then pstat_dac_set_code().
 */
esp_err_t pstat_dac_set_volt(float cell_v, const pstat_calib_t *cal);

/* ==========================================================================
 * ADC — ADS1115 on I2C (SDA=21, SCL=22, addr=0x48)
 * Custom register-level driver — NOT a library.
 * GAIN_ONE (±4.096 V, 0.125 mV/bit = 125 nA/bit with 1 kΩ TIA).
 * Continuous mode on AIN1 (current hot-path); on-demand AIN0 (cell voltage).
 * First sample after channel switch discarded (~1.2-2.4 ms stale period @860 SPS).
 * ========================================================================== */

typedef enum {
    PSTAT_ADC_CURRENT = 0,  /**< AIN1 — IOUT_UC (TIA output → current). Continuous hot-path. */
    PSTAT_ADC_VOLTAGE = 1,  /**< AIN0 — VOUT_UC (cell voltage). On-demand with channel switch. */
} pstat_adc_channel_t;

/** Initialise ADS1115: probe I2C address, configure GAIN_ONE @860 SPS, start continuous on AIN1. */
esp_err_t pstat_adc_init(void);

/**
 * @brief  Switch the active ADS1115 channel.
 *         Switching rewrites the config register, restarts conversion, and marks the
 *         first sample stale (discarded automatically on next read).
 *         The HAL owns channel state — the electrochemistry algorithm never calls this.
 */
esp_err_t pstat_adc_select(pstat_adc_channel_t ch);

/** Read the latest conversion result (raw int16_t). Stale-sample protection applied. */
esp_err_t pstat_adc_read_raw(int16_t *out);

/**
 * @brief  Read current (µA) as the average of n_avg samples from AIN1 (continuous mode).
 *         Fastest path — AIN1 stays selected, no channel switch overhead.
 *         Called by the DPV algorithm at both the END of baseline and END of pulse.
 */
float pstat_adc_read_current_uA(uint8_t n_avg, const pstat_calib_t *cal);

/**
 * @brief  Read cell voltage (V) on-demand from AIN0.
 *         Switches channel, waits for a valid conversion, reads, switches back to AIN1.
 *         Called by the DPV algorithm once per step to emit the RE readback.
 */
float pstat_adc_read_cell_volt(const pstat_calib_t *cal);

/* ==========================================================================
 * CD4066 Electrode Multiplexer
 * T1=GPIO32, T2=GPIO25, T3=GPIO33 — exactly one HIGH at a time.
 * ========================================================================== */

/** Initialise mux GPIO outputs. All electrodes disconnected (all LOW) at init. */
esp_err_t pstat_mux_init(void);

/**
 * @brief  Select one electrode. Only the chosen line goes HIGH; the others go LOW.
 * @param  electrode  1, 2, or 3.
 */
esp_err_t pstat_mux_select(uint8_t electrode);

/** Disconnect all electrodes (T1/T2/T3 all LOW). Call before power-down or idle. */
esp_err_t pstat_mux_deselect_all(void);

/* ==========================================================================
 * Status LEDs
 * READY=GPIO12 (strapping: must NOT be HIGH at reset), PROCESSING=GPIO13.
 * ========================================================================== */

typedef enum {
    PSTAT_LED_READY      = 0,
    PSTAT_LED_PROCESSING = 1,
} pstat_led_t;

esp_err_t pstat_led_init(void);
esp_err_t pstat_led_set(pstat_led_t led, bool on);

/* ==========================================================================
 * Buttons — debounced via espressif/button (iot_button, pulled by esp_lvgl_port)
 * START=GPIO14 (active-low, INPUT_PULLUP).
 * NAV=GPIO0   (active-low, STRAPPING PIN — HIGH at boot, never hold at power-on).
 * ========================================================================== */

typedef void (*pstat_button_cb_t)(void *arg);

/** Initialise both button devices (iot_button). Must be called before registering callbacks. */
esp_err_t pstat_buttons_init(void);

/** Register a short-click callback on the START button (GPIO14). */
esp_err_t pstat_button_start_on_click(pstat_button_cb_t cb, void *arg);

/**
 * @brief  Register a short-click callback on the NAV button (GPIO0).
 *         Used by the LVGL encoder indev: short press = rotate focus (navigate).
 */
esp_err_t pstat_button_nav_on_click(pstat_button_cb_t cb, void *arg);

/**
 * @brief  Register a long-hold (≥10 s) callback on the NAV button (GPIO0).
 *         Used for factory-reset WiFi credentials (NVS erase).
 *         WARNING: GPIO0 is the ESP32 BOOT/strapping pin. Only safe to hold at RUNTIME,
 *         NOT during power-on or reset (that would enter flash-download mode).
 */
esp_err_t pstat_button_nav_on_factory_reset(pstat_button_cb_t cb, void *arg);

/* ==========================================================================
 * Self-test (P1 bringup application)
 * ========================================================================== */

/**
 * @brief  Run all HAL self-tests and print results via ESP_LOG.
 *         Tests: ADS1115 I2C probe, DAC linear ramp, mux isolation, LED blink, button ISRs.
 * @return ESP_OK if all pass, ESP_FAIL if any fail.
 */
esp_err_t pstat_hal_selftest(const pstat_calib_t *cal);

/* ==========================================================================
 * Convenience init (calls all HAL init functions in order)
 * ========================================================================== */

/**
 * @brief  Initialise all HAL subsystems: DAC → ADC → mux → LEDs → buttons.
 *         Call once at boot before any other pstat_* function.
 * @return ESP_OK if all subsystems initialise; first error code otherwise.
 */
esp_err_t pstat_hal_init_all(void);

/* ==========================================================================
 * Dependency-injection: bind HAL functions to echem_core hal_callbacks_t
 * ========================================================================== */

/**
 * @brief  Build a hal_callbacks_t struct wired to the real HAL functions.
 *
 * The algorithm (dpv_run etc.) calls ONLY the callbacks — it never calls
 * pstat_* functions directly. This is the DI seam that keeps echem_core
 * hardware-free and host-testable.
 *
 * @param  cal  Calibration constants (pstat_calib_t) used by the callbacks.
 *              Pointer must remain valid for the lifetime of the returned struct.
 * @return      hal_callbacks_t with set_voltage, read_current_uA, read_cell_volt,
 *              delay_ms bound; emit_point and check_abort are NULL (wired by
 *              acq_engine in P3).
 *
 * Compile-time note: this function will not compile if HAL function signatures
 * mismatch hal_callbacks_t typedefs — that IS the intended compile check.
 */
hal_callbacks_t pstat_make_hal_callbacks(pstat_calib_t *cal);

#ifdef __cplusplus
}
#endif
