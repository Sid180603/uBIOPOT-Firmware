cmake_minimum_required(VERSION 3.16)
project(ubiopot_host_test C)
set(CMAKE_C_STANDARD 99)

# ==============================================================================
# Unity test framework — fetched from GitHub at configure time.
# Requires internet access on first run.
# Behind a proxy: set GIT_PROXY env var, or clone Unity manually:
#   git clone --depth 1 --branch v2.6.0 https://github.com/ThrowTheSwitch/Unity.git vendor/Unity
# then replace FetchContent_MakeAvailable(unity) with:
#   add_subdirectory(vendor/Unity)
# ==============================================================================
include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)

# ==============================================================================
# echem_core sources (pure C — no IDF headers).
# The testability invariant: these compile with host GCC, no ESP32 toolchain.
# Any accidental inclusion of esp_*.h would break this build and catch the violation.
# ==============================================================================
set(ECHEM_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/echem_core")

set(ECHEM_CORE_SRCS
    "${ECHEM_CORE_DIR}/src/calibration.c"
    "${ECHEM_CORE_DIR}/src/dpv.c"
    "${ECHEM_CORE_DIR}/src/peaks.c"
    "${ECHEM_CORE_DIR}/src/protocol.c"
    "${ECHEM_CORE_DIR}/src/technique.c"
)

# Convenience function: create one test executable per test suite.
# ctest runs each binary independently so failures are isolated.
function(add_unity_test TARGET SOURCE)
    add_executable(${TARGET} ${SOURCE} ${ECHEM_CORE_SRCS})
    target_include_directories(${TARGET} PRIVATE
        "${ECHEM_CORE_DIR}/include"
        "${unity_SOURCE_DIR}/src"
    )
    target_link_libraries(${TARGET} PRIVATE unity m)
    # Enable all warnings to catch echem_core quality issues early.
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${TARGET} COMMAND ${TARGET})
endfunction()

enable_testing()

add_unity_test(test_calibration  test_calibration.c)
add_unity_test(test_dpv          test_dpv.c)
add_unity_test(test_protocol     test_protocol.c)
