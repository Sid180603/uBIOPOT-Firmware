#include "pstat_hal/pstat_hal.h"
#include "esp_log.h"
/* TODO P1: #include "driver/spi_master.h" */

static const char *TAG = "hal_dac";

/*
 * TODO P1: MCP4921 12-bit SPI DAC driver.
 *
 * Hardware: SPI3_HOST (VSPI), CS=GPIO26, SCK=GPIO18, MOSI=GPIO23, MISO=-1.
 * SPI mode 0 (CPOL=0, CPHA=0). Max clock: 20 MHz.
 *
 * SPI word format (16-bit, MSB first):
 *   Bit 15   : ~CS (always 0 when we write)
 *   Bit 14   : BUF (0 = unbuffered)
 *   Bit 13   : ~GA (0 = 1x gain, Vout = Vref * D/4096)
 *   Bit 12   : ~SHDN (0 = active output)
 *   Bits 11:0: 12-bit DAC code
 *   => word = 0x3000 | (code & 0x0FFF)
 *
 * P1 implementation steps:
 *   1. spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO)
 *   2. spi_bus_add_device(SPI3_HOST, &dev_cfg, &s_dac_handle)
 *   3. pstat_dac_set_code: spi_transaction_t with tx_data = {word_hi, word_lo}
 */

esp_err_t pstat_dac_init(void)
{
    ESP_LOGW(TAG, "stub — not implemented (P1)");
    return ESP_OK;
}

esp_err_t pstat_dac_set_code(uint16_t code)
{
    (void)code;
    return ESP_OK;
}

esp_err_t pstat_dac_set_volt(float cell_v, const pstat_calib_t *cal)
{
    (void)cell_v;
    (void)cal;
    return ESP_OK;
}
