## echem_core — PURE C component. NO esp_*/FreeRTOS headers permitted.
## This invariant is what makes host-unit-testing possible.
## Verify at any time: grep -r "esp_" components/echem_core/include components/echem_core/src
## Expected result: zero matches.

idf_component_register(
    SRCS
        "src/calibration.c"
        "src/dpv.c"
        "src/peaks.c"
        "src/protocol.c"
        "src/scan_state.c"
        "src/technique.c"
    INCLUDE_DIRS "include"
    ## NO REQUIRES — only standard C library (newlib), automatically available.
    ## HW components (pstat_hal, acq_engine, ...) depend on echem_core; never the reverse.
)
