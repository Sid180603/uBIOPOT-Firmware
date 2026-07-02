## pstat_hal — Hardware Abstraction Layer
## REQUIRES echem_core (for pstat_calib_t and hal_callbacks_t types).
## Driver deps listed as PRIV_REQUIRES (used internally in .c files, not exposed via headers).
## Internal DAC/ADC fallback dropped (architecturally dead on OP07 board — see plan.md OPEN ITEM 4).

idf_component_register(
    SRCS
        "src/hal_dac.c"
        "src/hal_adc.c"
        "src/hal_mux.c"
        "src/hal_io.c"
    INCLUDE_DIRS "include"
    REQUIRES echem_core
    PRIV_REQUIRES
        esp_driver_spi
        esp_driver_i2c
        esp_driver_gpio
        log
)
