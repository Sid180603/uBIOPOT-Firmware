#include "pstat_hal/pstat_hal.h"
#include "echem_core/calibration.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "hal_adc";

/* ADS1115 I2C address (ADDR pin tied to GND) */
#define ADS1115_ADDR     0x48u

/* Register pointers */
#define ADS_REG_CONV     0x00u  /* Conversion register (read, int16_t big-endian) */
#define ADS_REG_CONF     0x01u  /* Configuration register (read/write, 16-bit) */

/*
 * Config register values — GAIN_ONE (±4.096 V, 0.125 mV/bit = 125 nA/bit with 1 kΩ TIA),
 * continuous mode, 860 SPS, comparator disabled.
 *
 * Bit layout:
 *   OS=0, MUX[2:0], PGA[2:0]=001, MODE=0, DR[2:0]=111, COMP_MODE=0, COMP_POL=0,
 *   COMP_LAT=0, COMP_QUE[1:0]=11
 *
 * AIN0 (voltage): MUX=100=0x4000  → config = 0x42E3
 * AIN1 (current): MUX=101=0x5000  → config = 0x52E3
 */
#define ADS_CONF_AIN0    0x42E3u   /* AIN0/GND, GAIN_ONE, continuous, 860 SPS */
#define ADS_CONF_AIN1    0x52E3u   /* AIN1/GND, GAIN_ONE, continuous, 860 SPS */

/* One conversion period at 860 SPS = 1/860 ≈ 1.16 ms.
 * After a channel switch, discard the first sample (still from the old channel).
 * Wait 3 ms (~2.6 conversion periods) to ensure a fresh result. */
#define ADS_STALE_WAIT_MS  3u

static i2c_master_bus_handle_t  s_bus_handle = NULL;
static i2c_master_dev_handle_t  s_dev_handle = NULL;
static pstat_adc_channel_t      s_current_ch  = PSTAT_ADC_CURRENT;
static bool                     s_stale        = false;

/* ---- Low-level register helpers ---- */

static esp_err_t ads_write_config(uint16_t config)
{
    uint8_t buf[3] = {
        ADS_REG_CONF,
        (uint8_t)(config >> 8),
        (uint8_t)(config & 0xFFu)
    };
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), 100);
}

static esp_err_t ads_read_conversion(int16_t *out)
{
    uint8_t reg  = ADS_REG_CONV;
    uint8_t data[2];
    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, 2, 100);
    if (ret == ESP_OK) {
        *out = (int16_t)((data[0] << 8) | data[1]);
    }
    return ret;
}

/* ---- Public HAL functions ---- */

esp_err_t pstat_adc_init(void)
{
    /* Create I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = I2C_NUM_0,
        .sda_io_num             = CONFIG_UBIOPOT_ADC_SDA,
        .scl_io_num             = CONFIG_UBIOPOT_ADC_SCL,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add ADS1115 device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = CONFIG_UBIOPOT_ADC_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Probe: confirm ADS1115 is on the bus */
    ret = i2c_master_probe(s_bus_handle, ADS1115_ADDR, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 probe FAILED (0x%02X) — check wiring: SDA=GPIO%d, SCL=GPIO%d",
                 ADS1115_ADDR, CONFIG_UBIOPOT_ADC_SDA, CONFIG_UBIOPOT_ADC_SCL);
        return ret;
    }

    /* Start continuous conversion on AIN1 (current hot-path) */
    ret = ads_write_config(ADS_CONF_AIN1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 config write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_current_ch = PSTAT_ADC_CURRENT;
    s_stale      = true;  /* discard first sample after power-on */

    ESP_LOGI(TAG, "ADS1115 ADC init OK — I2C addr=0x%02X, SDA=GPIO%d, SCL=GPIO%d, "
             "GAIN_ONE (±4.096 V, 125 nA/bit), 860 SPS",
             ADS1115_ADDR, CONFIG_UBIOPOT_ADC_SDA, CONFIG_UBIOPOT_ADC_SCL);
    return ESP_OK;
}

esp_err_t pstat_adc_select(pstat_adc_channel_t ch)
{
    if (ch == s_current_ch) {
        return ESP_OK;  /* already selected — no-op */
    }

    uint16_t config = (ch == PSTAT_ADC_CURRENT) ? ADS_CONF_AIN1 : ADS_CONF_AIN0;
    esp_err_t ret   = ads_write_config(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pstat_adc_select(%d) config write failed", (int)ch);
        return ret;
    }
    s_current_ch = ch;
    s_stale      = true;  /* first sample after MUX switch is from old channel — discard */
    return ESP_OK;
}

esp_err_t pstat_adc_read_raw(int16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    /* If channel was just switched, wait for a fresh conversion */
    if (s_stale) {
        vTaskDelay(pdMS_TO_TICKS(ADS_STALE_WAIT_MS));
        s_stale = false;
    }

    return ads_read_conversion(out);
}

float pstat_adc_read_current_uA(uint8_t n_avg, const pstat_calib_t *cal)
{
    /* AIN1 should already be the active channel (hot-path, no switch overhead).
     * Handle stale first sample if this is the very first call after init/switch. */
    if (s_stale) {
        vTaskDelay(pdMS_TO_TICKS(ADS_STALE_WAIT_MS));
        s_stale = false;
    }

    if (n_avg == 0) n_avg = 1;

    int32_t  sum   = 0;
    uint8_t  valid = 0;
    for (uint8_t i = 0; i < n_avg; i++) {
        int16_t raw = 0;
        if (ads_read_conversion(&raw) == ESP_OK) {
            sum += raw;
            valid++;
        }
        /* Small inter-sample gap ensures independent conversions at 860 SPS. */
        if (i < n_avg - 1) {
            vTaskDelay(pdMS_TO_TICKS(2)); /* ~1.7 conversion periods @860 SPS */
        }
    }

    if (valid == 0) return 0.0f;

    /*
     * Keep the float average intact — do NOT cast to int16_t.
     * Casting would truncate the fractional part (e.g. 15960.4 → 15960),
     * discarding the sub-LSB information that n_avg averaging provides.
     * With n_avg=5 we get ~2.2× noise reduction (sqrt(5)); truncation
     * reduces that to near-zero benefit (±125 nA lost for Rf=1 kΩ).
     *
     * Inline the raw→volt conversion to stay in float throughout:
     *   vout = avg_raw * adc_lsb_uv * 1e-6  (same as calib_adc_raw_to_volt
     *          but operating on float avg_raw, not a truncated int16_t)
     */
    float avg_raw = (float)sum / (float)valid;
    float vout    = avg_raw * (cal->adc_lsb_uv * 1.0e-6f);
    return calib_vout_to_current_uA(vout, cal);
}

float pstat_adc_read_cell_volt(const pstat_calib_t *cal)
{
    /* Switch to voltage channel (AIN0) */
    pstat_adc_select(PSTAT_ADC_VOLTAGE);

    /* Read one sample (stale discard handled inside pstat_adc_read_raw) */
    int16_t raw = 0;
    pstat_adc_read_raw(&raw);

    /* Restore current channel (AIN1) for the next DPV step */
    pstat_adc_select(PSTAT_ADC_CURRENT);

    float vout = calib_adc_raw_to_volt(raw, cal);
    return calib_vout_to_cell_volt(vout, cal);
}
