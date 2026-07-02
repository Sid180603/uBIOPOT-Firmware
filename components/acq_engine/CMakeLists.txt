## acq_engine — FreeRTOS acquisition engine (P3).
## AcquisitionTask (Core-1) + DispatcherTask (Core-0) + command/dispatch queues +
## server-authoritative scan buffer + sink registry.

idf_component_register(
    SRCS "src/acq_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES echem_core pstat_hal freertos log
)
