## acq_engine skeleton — filled in P3.
## FreeRTOS: AcquisitionTask (Core-1), Dispatcher (Core-0), command queue,
## datapoint queue, ScanState machine, server-authoritative buffer, engine API.

idf_component_register(
    SRCS "src/acq_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES echem_core pstat_hal
)
