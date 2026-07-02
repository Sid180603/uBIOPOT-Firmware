#pragma once

/**
 * @file calibration.h
 * @brief Analog calibration constants and physical unit conversion functions.
 *
 * ALL constants are injected at runtime (from NVS in P8; compile defaults here for P0-P7).
 * No hardcoded magic numbers anywhere in the electrochemistry code.
 *
 * Current conversion sign verified from Blynk+TFT firmware (potentiostat_11012024.ino L299):
 *   actual_current = (vout2 - actual_current) * 1000;  // in uA  iout = (2-vout)/1k
 * Anodic current (oxidation) → vout < vref_mid → positive I_uA. ✓ (prof-verified Rf=1K)
 *
 * NOTE: NO esp_*.h or FreeRTOS headers in this file. Purity enforced by the P0 DoD grep check.
 */

#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Calibration constants struct
 * Injected into every technique run. P8 loads from NVS; P2 tests use compile defaults.
 * -------------------------------------------------------------------------- */

typedef struct {
    float    vref_mid;            /**< Virtual-ground midpoint (V). Measured from REF195 divider.
                                       Blynk+TFT used 1.995 (hand-tweaked); design intent = 2.0.
                                       Calibrate in P8 with a DMM. Default 1.995. */
    float    tia_feedback_ohms;   /**< TIA feedback resistor (Ω). R5 on OP07 schematic.
                                       Prof-verified = 1000 Ω. Calibrate gain correction in P8. */
    float    dac_vref;            /**< DAC full-scale reference voltage (V).
                                       MCP4921 on this board uses VDD (~3.3 V) as Vref.
                                       Measure precisely: typical 3.28–3.32 V. Default 3.3. */
    uint16_t dac_bits;            /**< DAC resolution bits. MCP4921 = 12. */
    float    adc_lsb_uv;          /**< ADC LSB size (µV). ADS1115 GAIN_ONE (±4.096 V) = 125 µV/bit.
                                       Best single-ended gain that fits the 1–3 V current signal
                                       without clipping. ~1.5× better than the old firmware default. */
    float    current_offset_uA;   /**< Zero-current offset (µA). Set by auto-zero routine (P8).
                                       Compensates TIA input offset + level-shifter (U7/U1) error. */
    float    current_gain;        /**< Current channel gain correction factor. Default 1.0.
                                       Trimmed in P8 bench calibration vs known current source. */
    float    voltage_offset_V;    /**< Cell voltage measurement offset (V). Default 0.0.
                                       Compensates U4/U5 level-shifter error. P8 bench-cal. */
} pstat_calib_t;

/** Default calibration constants for the OP07 board (best-known compile-time values). */
#define PSTAT_CALIB_DEFAULT {               \
    .vref_mid           = 1.995f,           \
    .tia_feedback_ohms  = 1000.0f,          \
    .dac_vref           = 3.3f,             \
    .dac_bits           = 12,               \
    .adc_lsb_uv         = 125.0f,           \
    .current_offset_uA  = 0.0f,             \
    .current_gain       = 1.0f,             \
    .voltage_offset_V   = 0.0f,             \
}

/* --------------------------------------------------------------------------
 * Unit conversion functions
 * -------------------------------------------------------------------------- */

/**
 * @brief  Convert a desired cell voltage (V) to a 12-bit DAC code.
 *
 * Formula: code = round((cell_v + vref_mid) / dac_vref * (2^dac_bits - 1))
 * Clamped to [0, 2^dac_bits - 1].
 *
 * @param  cell_v  Desired cell voltage (V). Practical range ≈ ±1 V around vref_mid.
 * @param  cal     Calibration constants.
 * @return         12-bit DAC code in [0, 4095].
 */
uint16_t calib_volt_to_dac(float cell_v, const pstat_calib_t *cal);

/**
 * @brief  Convert a DAC code back to cell voltage (V). Inverse of calib_volt_to_dac.
 */
float calib_dac_to_volt(uint16_t code, const pstat_calib_t *cal);

/**
 * @brief  Convert a raw ADS1115 int16_t reading to voltage (V).
 *
 * Formula: V = raw * (adc_lsb_uv * 1e-6)
 * For GAIN_ONE: adc_lsb_uv = 125 → 1 LSB = 125 µV.
 */
float calib_adc_raw_to_volt(int16_t raw, const pstat_calib_t *cal);

/**
 * @brief  Convert the TIA output voltage at ADS1115 AIN1 (V) to current (µA).
 *
 * Signal path: WE → U2 (TIA, Rf=1kΩ) → U7 (buffer) → U1 (level shifter) → AIN1.
 * Path is net non-inverting (verified from Blynk+TFT code convention).
 *
 * Formula: I(µA) = (vref_mid − vout) × (1e6 / tia_feedback_ohms) × gain + offset
 *
 * Sign: anodic current (oxidation, positive) → vout < vref_mid → I_uA > 0. ✓
 *
 * @param  vout    Voltage at ADS1115 AIN1 (V), measured via calib_adc_raw_to_volt.
 * @param  cal     Calibration constants.
 * @return         Current in µA. Positive = anodic (oxidation).
 */
float calib_vout_to_current_uA(float vout, const pstat_calib_t *cal);

/**
 * @brief  Convert a raw cell voltage reading at ADS1115 AIN0 (V) to cell potential (V).
 *
 * Applies voltage_offset_V correction.
 *
 * @param  vout    Voltage at ADS1115 AIN0 (V), measured via calib_adc_raw_to_volt.
 * @param  cal     Calibration constants.
 * @return         Cell potential (V) relative to virtual ground.
 */
float calib_vout_to_cell_volt(float vout, const pstat_calib_t *cal);

#ifdef __cplusplus
}
#endif
