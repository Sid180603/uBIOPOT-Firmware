/**
 * mcp4921.c — Wokwi custom chip: MCP4921 12-bit SPI DAC
 *
 * Hardware behaviour modelled:
 *   - SPI slave, mode 0 (CPOL=0, CPHA=0).
 *   - 16-bit write-only protocol.
 *   - Word format: [15:12] = config bits, [11:0] = DAC code.
 *     Config bits with 0x3000 (firmware default):
 *       bit15 = 0  → select DAC A (single channel device)
 *       bit14 = 0  → unbuffered VREF
 *       bit13 = 1  → output gain = 1× (VOUT = Vref × code / 4096)
 *       bit12 = 1  → SHDN = active (output enabled)
 *   - VREF tied to VDD (3.3 V in Wokwi sim).
 *   - LDAC tied to GND → output latches immediately on CS↑.
 *   - VOUT written as analog voltage via pin_analog_write().
 *
 * Compile:
 *   wokwi-cli chip compile chips/mcp4921.c -o chips/mcp4921.chip.wasm
 *
 * Reference:
 *   MCP4921 datasheet (Microchip DS21897B)
 *   Aqua-HMET pstat_hal/hal_dac.c — spi_transaction with 0x3000 | code
 */

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Wokwi DAC reference voltage (VDD = 3.3 V) ---- */
#define VREF_V   3.3f
#define DAC_BITS 12u
#define DAC_MAX  ((1u << DAC_BITS) - 1u)   /* 4095 */

typedef struct {
    pin_t   cs_pin;
    pin_t   vout_pin;
    spi_dev_t spi;

    uint8_t  rx_buf[2];    /* 2-byte SPI word */
    float    vout_v;       /* current output voltage */
} mcp4921_t;

/* ---- Forward declarations ---- */
static void on_cs_change(void *user_data, pin_t pin, uint32_t value);
static void on_spi_done(void *user_data, uint8_t *buffer, uint32_t count);

void chip_init(void) {
    mcp4921_t *chip = (mcp4921_t *)malloc(sizeof(mcp4921_t));
    memset(chip, 0, sizeof(*chip));

    /* Analog output pin */
    chip->vout_pin = pin_init("VOUT", ANALOG);
    pin_dac_write(chip->vout_pin, 0.0f);
    chip->vout_v = 0.0f;

    /* CS pin — watch for falling edge to start SPI transaction */
    chip->cs_pin = pin_init("CS", INPUT_PULLUP);
    pin_watch_config_t cs_cfg = {
        .edge       = BOTH,
        .pin_change = on_cs_change,
        .user_data  = chip,
    };
    pin_watch(chip->cs_pin, &cs_cfg);

    /* SPI — SCK + MOSI (no MISO: MCP4921 is write-only) */
    spi_config_t spi_cfg = {
        .sck       = pin_init("SCK",  INPUT),
        .mosi      = pin_init("SDI",  INPUT),
        .miso      = NO_PIN,
        .mode      = 0,
        .done      = on_spi_done,
        .user_data = chip,
    };
    chip->spi = spi_init(&spi_cfg);

    printf("[MCP4921] chip_init OK — VREF=%.2f V, %u-bit DAC\n", VREF_V, DAC_BITS);
}

static void on_cs_change(void *user_data, pin_t pin, uint32_t value) {
    mcp4921_t *chip = (mcp4921_t *)user_data;
    if (value == 0) {
        /* CS low → start 2-byte SPI transaction */
        memset(chip->rx_buf, 0, sizeof(chip->rx_buf));
        spi_start(chip->spi, chip->rx_buf, sizeof(chip->rx_buf));
    } else {
        /* CS high → latch DAC output (LDAC=GND, so output latches immediately) */
        spi_stop(chip->spi);
    }
}

static void on_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
    mcp4921_t *chip = (mcp4921_t *)user_data;

    if (count < 2) {
        /* Partial transfer — keep listening */
        if (pin_read(chip->cs_pin) == 0) {
            spi_start(chip->spi, chip->rx_buf, sizeof(chip->rx_buf));
        }
        return;
    }

    /* Reconstruct 16-bit word (MSB first) */
    uint16_t word = ((uint16_t)buffer[0] << 8) | buffer[1];

    /* Decode config bits */
    uint8_t  shdn  = (word >> 12) & 1u;   /* bit12: 1 = active */
    uint8_t  ga    = (word >> 13) & 1u;   /* bit13: 1 = 1× gain */
    uint16_t code  = word & 0x0FFFu;       /* bits [11:0] */

    if (!shdn) {
        /* SHDN = 0 → output disabled */
        pin_dac_write(chip->vout_pin, 0.0f);
        chip->vout_v = 0.0f;
        return;
    }

    /* Output voltage:
     *   1× gain (bit13=1): VOUT = VREF × code / 4096
     *   2× gain (bit13=0): VOUT = 2 × VREF × code / 4096  (clamped to VREF)
     */
    float gain = ga ? 1.0f : 2.0f;
    float v    = gain * VREF_V * (float)code / 4096.0f;
    if (v > VREF_V) v = VREF_V;
    if (v < 0.0f)   v = 0.0f;

    pin_dac_write(chip->vout_pin, v);
    chip->vout_v = v;

    printf("[MCP4921] SPI word=0x%04X  code=%u  VOUT=%.4f V\n", word, code, v);

    /* If CS is still low, queue next transaction (shouldn't happen for MCP4921) */
    if (pin_read(chip->cs_pin) == 0) {
        spi_start(chip->spi, chip->rx_buf, sizeof(chip->rx_buf));
    }
}
