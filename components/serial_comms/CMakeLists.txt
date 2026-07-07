## serial_comms — P7 USB-serial NDJSON protocol.
## Registers as an engine sink; streams DataPoints as NDJSON lines + JSON events.
## Accepts JSON commands over UART0: start / abort / zero / state / hello.

idf_component_register(
    SRCS "src/serial_comms.c"
    INCLUDE_DIRS "include"
    REQUIRES
        echem_core
        acq_engine
        esp_driver_uart
        json
        freertos
        log
        esp_common
)
