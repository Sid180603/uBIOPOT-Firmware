idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        echem_core
        pstat_hal
        acq_engine
        ui_tft
        net_comms
        esp_log
)
