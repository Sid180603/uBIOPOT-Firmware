#include "pstat_hal/pstat_hal.h"
#include "echem_core/calibration.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "hal_dac";

/*
 * MCP4921 12-bit SPI DAC driver.
 *
 * Hardware: SPI3_HOST (VSPI), CS=GPIO26, SCK=GPIO18, MOSI=GPIO23, MISO=-1.
 * SPI mode 0 (CPOL=0, CPHA=0). Max SPI clock: 20 MHz (using 10 MHz for safety).
 *
 * SPI word format (16-bit, MSB first):
 *   Bits 15-12 = 0011  (BUF=0, GA=1x gain, SHDN=active output)
 *   Bits 11-0  = 12-bit DAC code
 *   => word = 0x3000 | (code & 0x0FFF)
 *
 * Vout = (code / 4095) * dac_vref  (dac_vref = VDD ≈ 3.3 V on this board)
 * Cell voltage E = Vout - vref_mid  (vref_mid ≈ 1.995 V)
 * volt_to_dac(0 V) → code ≈ 2476 → Vout ≈ 1.995 V → E = 0 V  ✓
 */

static spi_device_handle_t s_spi_handle = NULL;

esp_err_t pstat_dac_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = CONFIG_UBIOPOT_DAC_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = CONFIG_UBIOPOT_DAC_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits    = 0,
        .address_bits    = 0,
        .dummy_bits      = 0,
        .mode            = 0,                /* SPI mode 0: CPOL=0, CPHA=0 */
        .clock_speed_hz  = 10 * 1000 * 1000, /* 10 MHz — conservative for PCB routing */
        .spics_io_num    = CONFIG_UBIOPOT_DAC_CS,
        .queue_size      = 1,
    };

    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Park DAC at midpoint → 0 V cell voltage */
    pstat_calib_t cal = PSTAT_CALIB_DEFAULT;
    pstat_dac_set_volt(0.0f, &cal);

    ESP_LOGI(TAG, "MCP4921 DAC init OK — SPI3, CS=GPIO%d, SCK=GPIO%d, MOSI=GPIO%d",
             CONFIG_UBIOPOT_DAC_CS, CONFIG_UBIOPOT_DAC_SCK, CONFIG_UBIOPOT_DAC_MOSI);
    return ESP_OK;
}

esp_err_t pstat_dac_set_code(uint16_t code)
{
    if (!s_spi_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * SPI_TRANS_USE_TXDATA: data stored inline (≤4 bytes), no DMA.
     * ESP32 sends tx_data[0] first (MSB of word), then tx_data[1].
     */
    uint16_t word = (uint16_t)(0x3000u | (code & 0x0FFFu));
    spi_transaction_t t = {
        .length  = 16,
        .flags   = SPI_TRANS_USE_TXDATA,
        .tx_data = {
            (uint8_t)(word >> 8),
            (uint8_t)(word & 0xFFu),
            0u, 0u
        },
    };
    return spi_device_transmit(s_spi_handle, &t);
}

esp_err_t pstat_dac_set_volt(float cell_v, const pstat_calib_t *cal)
{
    return pstat_dac_set_code(calib_volt_to_dac(cell_v, cal));
}
