/**
 * ads1115.c — Wokwi custom chip: ADS1115 16-bit I2C ADC
 *             with built-in electrochemical cell simulator
 *
 * Hardware behaviour modelled:
 *   - I2C slave at address 0x48 (ADDR pin tied to GND).
 *   - Two-register protocol:
 *       Pointer 0x00 = Conversion register (16-bit, read)
 *       Pointer 0x01 = Config register    (16-bit, write/read)
 *   - Config register layout (ADS1115 datasheet §8.6.3):
 *       bits[15]    = OS         (write 1 to start single-shot; ignored in continuous)
 *       bits[14:12] = MUX[2:0]  (000=AIN0, 001=AIN1, ...)  ← single-ended vs GND
 *       bits[11:9]  = PGA[2:0]  (001 = GAIN_ONE ±4.096 V, LSB = 0.125 mV)
 *       bits[8]     = MODE      (0 = continuous, 1 = single-shot)
 *       bits[7:5]   = DR[2:0]   (111 = 860 SPS)
 *       bits[4:0]   = comparator config (not used here)
 *   - Firmware uses GAIN_ONE (PGA=001) and continuous mode (MODE=0).
 *
 * Cell simulator (shared Gaussian DPV model):
 *   Same model as sim/main_sim.c and test/mock_ws_server.py for consistency.
 *   AIN1 (current channel):
 *     1. Read V_dac from AIN0 pin (analog wire from MCP4921 VOUT).
 *     2. E_mV = (V_dac − VREF_MID) × 1000   (firmware convention)
 *     3. I_uA = Gaussian peaks (Cd/Pb/Cu/Hg) centred at known E_peak potentials.
 *     4. V_TIA_out = VREF_MID − I_uA × TIA_RF_OHMS/1e6
 *        (TIA sign: firmware expects (VREF_MID − Vout) × 1e6/Rf = I_uA)
 *     5. Return ADC count = round(V_TIA_out / ADC_FSR × 32768)
 *   AIN0 (voltage channel):
 *     Returns ADC count for the DAC output voltage (V_dac / ADC_FSR × 32768).
 *
 * I2C transaction protocol implemented here:
 *   WRITE: [addr_W] [pointer] [MSB] [LSB]  → sets register pointer and optionally writes
 *   READ:  [addr_R]                         → returns 2 bytes from register at last pointer
 *
 * Compile:
 *   wokwi-cli chip compile chips/ads1115.c -o chips/ads1115.chip.wasm
 *
 * Reference:
 *   ADS1115 datasheet (Texas Instruments SBAS444D)
 *   Aqua-HMET pstat_hal/hal_adc.c
 */

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ---- Calibration constants (match pstat_hal defaults) ---- */
#define VREF_MID       1.995f      /* 2V virtual ground (REF195 divider) */
#define TIA_RF_OHMS    1000.0f     /* TIA feedback resistor (Rf = 1 kΩ)  */
#define ADC_FSR        4.096f      /* Full-scale range for GAIN_ONE (±4.096 V) */
#define ADC_COUNTS     32768       /* 2^15 (signed 16-bit) */

/* I2C address (ADDR → GND) */
#define I2C_ADDR  0x48u

/* ADS1115 register pointers */
#define REG_CONV    0x00u
#define REG_CONFIG  0x01u

/* MUX field in config register (bits [14:12]) */
#define MUX_AIN0    0x4u   /* 100 = AIN0 single-ended vs GND */
#define MUX_AIN1    0x5u   /* 101 = AIN1 single-ended vs GND */

/* ---- Synthetic DPV peaks (Gaussian model) ---- */
/* Same parameters as sim/main_sim.c + test/mock_ws_server.py */
typedef struct { float E_peak; float I_peak; float sigma; } peak_t;

static const peak_t PEAKS[] = {
    { -700.0f, 45.0f, 60.0f },   /* Cd²⁺ ≈ −0.7 V */
    { -400.0f, 30.0f, 50.0f },   /* Pb²⁺ ≈ −0.4 V */
    {    0.0f, 20.0f, 55.0f },   /* Cu²⁺ ≈  0.0 V */
    {  300.0f, 10.0f, 45.0f },   /* Hg²⁺ ≈ +0.3 V */
};
#define N_PEAKS (sizeof(PEAKS) / sizeof(PEAKS[0]))

static float synthetic_current_uA(float E_mV) {
    float I = 0.0f;
    for (int i = 0; i < (int)N_PEAKS; i++) {
        float dE = E_mV - PEAKS[i].E_peak;
        I += PEAKS[i].I_peak * expf(-0.5f * (dE / PEAKS[i].sigma) * (dE / PEAKS[i].sigma));
    }
    /* Small deterministic baseline (no randomness for reproducibility) */
    I += 0.5f * sinf(E_mV * 0.01f);
    return I;
}

/* ---- Chip state ---- */
typedef struct {
    i2c_dev_t i2c;
    pin_t ain0_pin;    /* reads V_dac from MCP4921 VOUT (analog wire) */
    pin_t ain1_pin;    /* optional direct connection; we mainly use ain0_pin */

    uint8_t  reg_ptr;          /* current register pointer */
    uint16_t config_reg;       /* last written config register */

    /* I2C transaction state */
    uint8_t  wr_buf[3];        /* [ptr, MSB, LSB] */
    uint8_t  wr_idx;
    bool     is_read;
    uint8_t  rd_buf[2];        /* MSB, LSB of conversion result */
    uint8_t  rd_idx;
} ads1115_t;

/* ---- Helper: compute 16-bit conversion result for current MUX setting ---- */
static int16_t compute_conversion(ads1115_t *chip) {
    uint8_t mux = (chip->config_reg >> 12) & 0x7u;

    /* Read DAC output voltage from AIN0 analog pin (connected to MCP4921 VOUT) */
    float v_dac = pin_adc_read(chip->ain0_pin);

    if (mux == MUX_AIN0) {
        /* Voltage channel: return ADC count proportional to V_dac */
        float count_f = (v_dac / ADC_FSR) * (float)ADC_COUNTS;
        if (count_f >  32767.0f) count_f =  32767.0f;
        if (count_f < -32768.0f) count_f = -32768.0f;
        return (int16_t)count_f;

    } else {
        /* Current channel (AIN1 or default): apply cell simulator */
        float E_mV      = (v_dac - VREF_MID) * 1000.0f;
        float I_uA      = synthetic_current_uA(E_mV);

        /* TIA output voltage: V_TIA = VREF_MID − I_uA × Rf/1e6
         * (firmware sign convention: I = (VREF_MID − V_TIA) × 1e6 / Rf)  */
        float v_tia     = VREF_MID - I_uA * (TIA_RF_OHMS / 1e6f);
        if (v_tia < 0.0f)     v_tia = 0.0f;
        if (v_tia > ADC_FSR)  v_tia = ADC_FSR;

        float count_f   = (v_tia / ADC_FSR) * (float)ADC_COUNTS;
        if (count_f >  32767.0f) count_f =  32767.0f;
        if (count_f < -32768.0f) count_f = -32768.0f;

        return (int16_t)count_f;
    }
}

/* ---- I2C callbacks ---- */

static bool on_connect(void *user_data, uint32_t address, bool read) {
    ads1115_t *chip = (ads1115_t *)user_data;
    chip->is_read = read;
    chip->wr_idx  = 0;
    chip->rd_idx  = 0;

    if (read) {
        /* Prepare conversion data MSB-first */
        int16_t conv = compute_conversion(chip);
        chip->rd_buf[0] = (uint8_t)(((uint16_t)conv) >> 8);
        chip->rd_buf[1] = (uint8_t)(((uint16_t)conv) & 0xFFu);
    }
    return true;  /* ACK */
}

static uint8_t on_read(void *user_data) {
    ads1115_t *chip = (ads1115_t *)user_data;
    if (chip->rd_idx < 2) {
        return chip->rd_buf[chip->rd_idx++];
    }
    return 0xFF;
}

static bool on_write(void *user_data, uint8_t data) {
    ads1115_t *chip = (ads1115_t *)user_data;

    if (chip->wr_idx == 0) {
        /* First byte = register pointer */
        chip->reg_ptr = data & 0x03u;
        chip->wr_idx  = 1;
        return true;
    }

    if (chip->reg_ptr == REG_CONFIG) {
        if (chip->wr_idx == 1) {
            chip->config_reg = (uint16_t)(data << 8);
            chip->wr_idx = 2;
        } else if (chip->wr_idx == 2) {
            chip->config_reg |= data;
            uint8_t mux = (chip->config_reg >> 12) & 0x7u;
            uint8_t pga = (chip->config_reg >>  9) & 0x7u;
            printf("[ADS1115] Config=0x%04X  MUX=%u  PGA=%u\n",
                   chip->config_reg, mux, pga);
            chip->wr_idx = 3;
        }
    } else {
        chip->wr_idx++;
    }
    return true;
}

static void on_disconnect(void *user_data) {
    /* nothing */
    (void)user_data;
}

/* ---- chip_init ---- */
void chip_init(void) {
    ads1115_t *chip = (ads1115_t *)malloc(sizeof(ads1115_t));
    memset(chip, 0, sizeof(*chip));

    /* Default config: MUX=AIN1, GAIN_ONE, continuous, 860 SPS */
    chip->config_reg = 0xC583u;  /* OS=1,MUX=101,PGA=001,MODE=0,DR=111,COMP_MODE=0 */
    chip->reg_ptr    = REG_CONV;

    /* Analog input pins (read DAC voltage from MCP4921 VOUT via analog wire) */
    chip->ain0_pin = pin_init("AIN0", ANALOG);
    chip->ain1_pin = pin_init("AIN1", ANALOG);

    /* I2C slave */
    i2c_config_t i2c_cfg = {
        .address    = I2C_ADDR,
        .scl        = pin_init("SCL", INPUT_PULLUP),
        .sda        = pin_init("SDA", INPUT_PULLUP),
        .connect    = on_connect,
        .read       = on_read,
        .write      = on_write,
        .disconnect = on_disconnect,
        .user_data  = chip,
    };
    chip->i2c = i2c_init(&i2c_cfg);

    printf("[ADS1115] chip_init OK — addr=0x%02X  GAIN_ONE ±%.3f V\n",
           I2C_ADDR, ADC_FSR);
}
