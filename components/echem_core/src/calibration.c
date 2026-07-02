#include "echem_core/calibration.h"
#include <math.h>
#include <stddef.h>

uint16_t calib_volt_to_dac(float cell_v, const pstat_calib_t *cal)
{
    /*
     * Map cell_v (V) to a DAC code.
     * At cell_v = 0V, the DAC should output vref_mid (the virtual ground).
     * At cell_v = +1V (max anodic), DAC outputs vref_mid + 1.0V.
     * At cell_v = -1V (max cathodic), DAC outputs vref_mid - 1.0V.
     *
     * code = round((cell_v + vref_mid) / dac_vref * (2^dac_bits - 1))
     *
     * NOTE: the MCP4921 takes VDD as Vref when VREFA/B=0 (this board config).
     * dac_vref should be measured as the actual VDD (typically 3.28–3.32 V).
     */
    float full_scale = (float)((1u << cal->dac_bits) - 1u);
    float code_f     = roundf((cell_v + cal->vref_mid) / cal->dac_vref * full_scale);
    if (code_f < 0.0f)        return 0u;
    if (code_f > full_scale)  return (uint16_t)full_scale;
    return (uint16_t)code_f;
}

float calib_dac_to_volt(uint16_t code, const pstat_calib_t *cal)
{
    float full_scale = (float)((1u << cal->dac_bits) - 1u);
    return ((float)code / full_scale) * cal->dac_vref - cal->vref_mid;
}

float calib_adc_raw_to_volt(int16_t raw, const pstat_calib_t *cal)
{
    /* ADS1115 GAIN_ONE: 1 LSB = 125 µV = 0.000125 V */
    return (float)raw * (cal->adc_lsb_uv * 1.0e-6f);
}

float calib_vout_to_current_uA(float vout, const pstat_calib_t *cal)
{
    /*
     * Current path: WE → U2 (TIA, Rf=1kΩ) → U7 (buffer) → U1 (level shifter) → AIN1.
     * Net non-inverting (verified from Blynk+TFT source: actual_current=(vout2-vout)*1000).
     *
     * I_raw(µA) = (vref_mid - vout) * (1e6 / Rf)
     *           = (vref_mid - vout) * 1000   [for Rf=1kΩ]
     *
     * Sign: anodic current (oxidation) → WE sinks current → TIA output drops below vref_mid
     *       → (vref_mid - vout) > 0 → I_uA > 0. ✓
     *
     * Gain and offset correction are applied to trim for level-shifter error (P8 bench-cal).
     */
    float raw_uA = (cal->vref_mid - vout) * (1.0e6f / cal->tia_feedback_ohms);
    return raw_uA * cal->current_gain + cal->current_offset_uA;
}

float calib_vout_to_cell_volt(float vout, const pstat_calib_t *cal)
{
    /*
     * Voltage path: RE → U5 (follower) → U4 (level shifter) → AIN0.
     * vref_mid is the zero-potential reference.
     * Cell potential = measured voltage at AIN0 - vref_mid + voltage_offset_V.
     */
    return vout - cal->vref_mid + cal->voltage_offset_V;
}
