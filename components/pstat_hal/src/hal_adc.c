#include "pstat_hal/pstat_hal.h"
#include "esp_log.h"
/* TODO P1: #include "driver/i2c_master.h" */

static const char *TAG = "hal_adc";

/*
 * TODO P1: ADS1115 custom register-level I2C driver.
 *
 * Hardware: I2C, SDA=GPIO21, SCL=GPIO22, addr=0x48 (ADDR pin tied to GND).
 * ADS1115 I2C registers:
 *   0x00 = Conversion register (read, int16_t big-endian)
 *   0x01 = Config register (write to configure, read back to verify)
 *
 * Config register for CONTINUOUS mode, GAIN_ONE (±4.096 V), 860 SPS:
 *   AIN0: 0x44E3  (MUX=100=AIN0, PGA=001=GAIN_ONE, MODE=0=continuous, DR=111=860SPS, COMP disabled)
 *   AIN1: 0x54E3  (MUX=101=AIN1, rest same)
 *
 * CRITICAL: ADS1115 continuous mode converts ONLY the currently MUXed channel.
 * Channel switch = write new config → restarts conversion → first result is stale.
 * Stale sample discard: wait ~1.2 ms (1 conversion period @860 SPS) before reading.
 * Keep AIN1 (current) as the default continuous channel during DPV acquisition.
 * Switch to AIN0 (voltage) only for RE readback, then switch back.
 *
 * Key APIs to implement:
 *   i2c_new_master_bus()          — create I2C bus handle
 *   i2c_master_bus_add_device()   — add ADS1115 at 0x48
 *   i2c_master_probe()            — bringup connectivity check
 *   i2c_master_transmit()         — write config register (2+2 bytes: ptr + config word)
 *   i2c_master_transmit_receive() — set pointer=0x00, read 2-byte conversion (NO STOP between)
 *
 * P1 also implements stale-sample tracking:
 *   static bool s_first_sample_stale — set by pstat_adc_select(), cleared after first read.
 */

static pstat_adc_channel_t s_current_channel = PSTAT_ADC_CURRENT;

esp_err_t pstat_adc_init(void)
{
    ESP_LOGW(TAG, "stub — not implemented (P1)");
    return ESP_OK;
}

esp_err_t pstat_adc_select(pstat_adc_channel_t ch)
{
    s_current_channel = ch;
    return ESP_OK;
}

esp_err_t pstat_adc_read_raw(int16_t *out)
{
    if (out) *out = 0;
    return ESP_OK;
}

float pstat_adc_read_current_uA(uint8_t n_avg, const pstat_calib_t *cal)
{
    (void)n_avg;
    (void)cal;
    return 0.0f;
}

float pstat_adc_read_cell_volt(const pstat_calib_t *cal)
{
    (void)cal;
    return 0.0f;
}
