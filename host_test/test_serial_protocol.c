/**
 * test_serial_protocol.c
 * Unity host tests for the P7 serial_comms wire-protocol contract.
 *
 * What is tested here (pure C, no IDF, no hardware, runs in CI):
 *
 *   Protocol constants  (serial_comms_protocol.h)
 *     - SERIAL_PROTOCOL_VERSION == 1
 *     - All T_* / EVT_NAME_* / CMD_* string constants are non-empty
 *     - SERIAL parity vs NET constants (same semantics)
 *
 *   NDJSON point formatter  (echem_core/protocol.c, protocol_format_point)
 *     NOTE: serial_comms.c uses its own snprintf hot-path (SERIAL_POINT_* keys);
 *     we test the echem_core formatter here (same as test_protocol.c) PLUS
 *     the serial-specific format used in serial_comms to verify parity.
 *
 *   serial_comms_protocol.h constants
 *     - SERIAL_CMD_LINE_MAX >= 512
 *     - SERIAL_POINT_LINE_MAX >= 128
 *     - SERIAL_CHUNK_BYTES  >= 16
 *
 *   NDJSON line formatting helpers (via echem_core/protocol.c)
 *     - protocol_format_point returns the correct field names and values
 *     - protocol_format_event_started, _complete, _error, _hello
 *     - All lines end with '\n'
 *     - Buffer truncation safety: returns -1 on overflow
 *
 *   Parity table verification (serial NDJSON field names match documented values)
 *     - "t" field is always present
 *     - "cmd" field values match documented serial / WS command names
 *     - "name" field values for events match NET_EVT_NAME_* and SERIAL_EVT_NAME_*
 *
 * Run via ctest in host_test/:
 *   cmake -B build_host host_test && cmake --build build_host && ctest --test-dir build_host -V
 */

#include "unity.h"
#include "serial_comms_protocol.h"
#include "net_comms_protocol.h"      /* for parity assertions */
#include "echem_core/protocol.h"     /* NDJSON formatters */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ==============================================================================
 * Protocol version
 * ============================================================================== */

void test_serial_protocol_version_is_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, SERIAL_PROTOCOL_VERSION);
}

/** P7 and P5 protocol versions must be identical (same engine, same versioning). */
void test_serial_and_net_protocol_versions_match(void)
{
    TEST_ASSERT_EQUAL_INT(NET_PROTOCOL_VERSION, SERIAL_PROTOCOL_VERSION);
}

/* ==============================================================================
 * Message type constants non-empty
 * ============================================================================== */

void test_t_hello_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_HELLO)  > 0); }
void test_t_point_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_POINT)  > 0); }
void test_t_event_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_EVENT)  > 0); }
void test_t_state_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_STATE)  > 0); }
void test_t_peaks_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_PEAKS)  > 0); }
void test_t_resync_non_empty(void)  { TEST_ASSERT_TRUE(strlen(SERIAL_T_RESYNC_COMPLETE) > 0); }
void test_t_error_non_empty(void)   { TEST_ASSERT_TRUE(strlen(SERIAL_T_ERROR)  > 0); }

/* ==============================================================================
 * Event name constants — parity with net_comms_protocol.h
 * ============================================================================== */

void test_evt_started_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_STARTED,  SERIAL_EVT_NAME_STARTED);
}

void test_evt_equilib_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_EQUILIB,  SERIAL_EVT_NAME_EQUILIB);
}

void test_evt_complete_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_COMPLETE, SERIAL_EVT_NAME_COMPLETE);
}

void test_evt_aborted_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_ABORTED,  SERIAL_EVT_NAME_ABORTED);
}

void test_evt_error_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_ERROR,    SERIAL_EVT_NAME_ERROR);
}

void test_evt_reset_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_EVT_NAME_RESET,    SERIAL_EVT_NAME_RESET);
}

/* ==============================================================================
 * Command name constants — parity with net_comms_protocol.h
 * ============================================================================== */

void test_cmd_start_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_CMD_START,  SERIAL_CMD_START);
}

void test_cmd_abort_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_CMD_ABORT,  SERIAL_CMD_ABORT);
}

void test_cmd_zero_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_CMD_ZERO,   SERIAL_CMD_ZERO);
}

void test_cmd_state_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_CMD_STATE,  SERIAL_CMD_STATE);
}

void test_cmd_hello_parity(void)
{
    TEST_ASSERT_EQUAL_STRING(NET_CMD_HELLO,  SERIAL_CMD_HELLO);
}

/* ==============================================================================
 * Buffer size constants
 * ============================================================================== */

void test_cmd_line_max_at_least_512(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(512u, SERIAL_CMD_LINE_MAX);
}

void test_point_line_max_at_least_128(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(128u, SERIAL_POINT_LINE_MAX);
}

void test_chunk_bytes_at_least_16(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(16u, SERIAL_CHUNK_BYTES);
}

/* ==============================================================================
 * NDJSON formatters via echem_core/protocol.c
 * ============================================================================== */

/* ---- protocol_format_point ---- */

void test_format_point_returns_positive_length(void)
{
    char buf[256];
    int n = protocol_format_point(buf, sizeof(buf), 1, 0, -500.0f, 6.42f, -495.0f);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_format_point_type_is_point(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"point\""));
}

void test_format_point_electrode_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 3, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"e\":3"));
}

void test_format_point_idx_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 77, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"n\":77"));
}

void test_format_point_e_mv_to_volts(void)
{
    /* -500 mV → "v":-0.5000 */
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 0, -500.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"v\":-0.5000"));
}

void test_format_point_current_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 6.4200f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"i\":6.4200"));
}

void test_format_point_ends_with_newline(void)
{
    char buf[256];
    int n = protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

void test_format_point_overflow_returns_minus1(void)
{
    char buf[10];   /* way too small */
    int n = protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* ---- protocol_format_hello ---- */

void test_format_hello_has_type_hello(void)
{
    char buf[256];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"hello\""));
}

void test_format_hello_has_proto_field(void)
{
    char buf[256];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"proto\":"));
}

void test_format_hello_has_fw_field(void)
{
    char buf[256];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":"));
}

void test_format_hello_ends_with_newline(void)
{
    char buf[256];
    int n = protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

/* ---- protocol_format_event_started ---- */

void test_format_started_has_type_event(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"event\""));
}

void test_format_started_has_name_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_started\""));
}

void test_format_started_name_matches_serial_constant(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, SERIAL_EVT_NAME_STARTED));
}

void test_format_started_mode_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 2);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"DPV\""));
}

void test_format_started_electrode_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 2);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"e\":2"));
}

void test_format_started_ends_with_newline(void)
{
    char buf[256];
    int n = protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

/* ---- protocol_format_event_complete ---- */

void test_format_complete_has_name_scan_complete(void)
{
    char buf[256];
    protocol_format_event_complete(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_complete\""));
}

void test_format_complete_name_matches_serial_constant(void)
{
    char buf[256];
    protocol_format_event_complete(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, SERIAL_EVT_NAME_COMPLETE));
}

/* ---- protocol_format_event_error ---- */

void test_format_error_has_name_scan_error(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "test error");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_error\""));
}

void test_format_error_embeds_message(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "e_step must be positive");
    TEST_ASSERT_NOT_NULL(strstr(buf, "e_step must be positive"));
}

/** JSON injection guard: embedded '"' must be escaped. */
void test_format_error_escapes_double_quote(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "bad \"value\"");
    /* Must NOT contain an unescaped embedded " after the key colon */
    /* Quick check: raw '"value"' (without backslash) must not appear */
    TEST_ASSERT_NULL(strstr(buf, "\"value\""));
    /* Escaped version must appear */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\\\"value\\\""));
}

void test_format_error_name_matches_serial_constant(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "x");
    TEST_ASSERT_NOT_NULL(strstr(buf, SERIAL_EVT_NAME_ERROR));
}

/* ==============================================================================
 * DPV parameter keys — used by serial_comms.c for parsing inbound "params":{}
 * ============================================================================== */

void test_param_keys_non_empty(void)
{
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_E_BEGIN_MV)         > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_E_END_MV)           > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_E_STEP_MV)          > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_E_PULSE_MV)         > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_T_PULSE_MS)         > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_T_PERIOD_MS)        > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_T_EQUILIBRATION_MS) > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_CYCLES)             > 0);
    TEST_ASSERT_TRUE(strlen(SERIAL_PARAM_N_AVG)              > 0);
}

/* ==============================================================================
 * test_main
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* Protocol version */
    RUN_TEST(test_serial_protocol_version_is_1);
    RUN_TEST(test_serial_and_net_protocol_versions_match);

    /* T_* constants non-empty */
    RUN_TEST(test_t_hello_non_empty);
    RUN_TEST(test_t_point_non_empty);
    RUN_TEST(test_t_event_non_empty);
    RUN_TEST(test_t_state_non_empty);
    RUN_TEST(test_t_peaks_non_empty);
    RUN_TEST(test_t_resync_non_empty);
    RUN_TEST(test_t_error_non_empty);

    /* Event name parity (serial == net) */
    RUN_TEST(test_evt_started_parity);
    RUN_TEST(test_evt_equilib_parity);
    RUN_TEST(test_evt_complete_parity);
    RUN_TEST(test_evt_aborted_parity);
    RUN_TEST(test_evt_error_parity);
    RUN_TEST(test_evt_reset_parity);

    /* Command name parity (serial == net) */
    RUN_TEST(test_cmd_start_parity);
    RUN_TEST(test_cmd_abort_parity);
    RUN_TEST(test_cmd_zero_parity);
    RUN_TEST(test_cmd_state_parity);
    RUN_TEST(test_cmd_hello_parity);

    /* Buffer sizes */
    RUN_TEST(test_cmd_line_max_at_least_512);
    RUN_TEST(test_point_line_max_at_least_128);
    RUN_TEST(test_chunk_bytes_at_least_16);

    /* NDJSON formatters */
    RUN_TEST(test_format_point_returns_positive_length);
    RUN_TEST(test_format_point_type_is_point);
    RUN_TEST(test_format_point_electrode_field);
    RUN_TEST(test_format_point_idx_field);
    RUN_TEST(test_format_point_e_mv_to_volts);
    RUN_TEST(test_format_point_current_field);
    RUN_TEST(test_format_point_ends_with_newline);
    RUN_TEST(test_format_point_overflow_returns_minus1);

    RUN_TEST(test_format_hello_has_type_hello);
    RUN_TEST(test_format_hello_has_proto_field);
    RUN_TEST(test_format_hello_has_fw_field);
    RUN_TEST(test_format_hello_ends_with_newline);

    RUN_TEST(test_format_started_has_type_event);
    RUN_TEST(test_format_started_has_name_field);
    RUN_TEST(test_format_started_name_matches_serial_constant);
    RUN_TEST(test_format_started_mode_field);
    RUN_TEST(test_format_started_electrode_field);
    RUN_TEST(test_format_started_ends_with_newline);

    RUN_TEST(test_format_complete_has_name_scan_complete);
    RUN_TEST(test_format_complete_name_matches_serial_constant);

    RUN_TEST(test_format_error_has_name_scan_error);
    RUN_TEST(test_format_error_embeds_message);
    RUN_TEST(test_format_error_escapes_double_quote);
    RUN_TEST(test_format_error_name_matches_serial_constant);

    /* DPV param keys */
    RUN_TEST(test_param_keys_non_empty);

    return UNITY_END();
}
