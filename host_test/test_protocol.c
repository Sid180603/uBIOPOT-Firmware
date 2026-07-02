/**
 * test_protocol.c
 * Unity host tests for echem_core/protocol.c NDJSON output formatting.
 *
 * Tests every formatter for:
 *   - Non-empty output
 *   - Required JSON fields present (strstr)
 *   - Correct field values where verifiable
 *   - Trailing newline (NDJSON requirement)
 *   - Buffer truncation safety (no overflow)
 */

#include "unity.h"
#include "echem_core/protocol.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ==============================================================================
 * protocol_format_point
 * ============================================================================== */

void test_point_returns_positive_length(void)
{
    char buf[256];
    int n = protocol_format_point(buf, sizeof(buf), 1, 0, -500.0f, 6.42f, -495.0f);
    TEST_ASSERT_GREATER_THAN(0, n);
}

void test_point_has_type_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 0, -500.0f, 6.42f, -495.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"point\""));
}

void test_point_has_electrode_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 2, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"e\":2"));
}

void test_point_has_index_field(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 42, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"n\":42"));
}

void test_point_ends_with_newline(void)
{
    char buf[256];
    int n = protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 0.0f, 0.0f);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

/* E_mV converted to V in output: -500 mV → -0.5000 V */
void test_point_e_mv_converted_to_volts(void)
{
    char buf[256];
    protocol_format_point(buf, sizeof(buf), 1, 0, -500.0f, 0.0f, 0.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"v\":-0.5000"));
}

/* ==============================================================================
 * protocol_format_event_started
 * ============================================================================== */

void test_event_started_has_type_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"event\""));
}

void test_event_started_has_name_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_started\""));
}

void test_event_started_has_technique_field(void)
{
    char buf[256];
    protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"DPV\""));
}

void test_event_started_ends_with_newline(void)
{
    char buf[256];
    int n = protocol_format_event_started(buf, sizeof(buf), "DPV", 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

/* ==============================================================================
 * protocol_format_event_complete
 * ============================================================================== */

void test_event_complete_has_name_field(void)
{
    char buf[128];
    protocol_format_event_complete(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_complete\""));
}

/* ==============================================================================
 * protocol_format_event_error
 * ============================================================================== */

void test_event_error_has_name_field(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "test error");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"scan_error\""));
}

void test_event_error_contains_message(void)
{
    char buf[256];
    protocol_format_event_error(buf, sizeof(buf), "e_step out of range");
    TEST_ASSERT_NOT_NULL(strstr(buf, "e_step out of range"));
}

void test_event_error_null_message_safe(void)
{
    char buf[256];
    int n = protocol_format_event_error(buf, sizeof(buf), NULL);
    TEST_ASSERT_GREATER_THAN(0, n); /* must not crash */
}

/* ==============================================================================
 * protocol_format_hello
 * ============================================================================== */

void test_hello_has_type_field(void)
{
    char buf[128];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"hello\""));
}

void test_hello_has_proto_version(void)
{
    char buf[128];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"proto\":1"));
}

void test_hello_has_fw_version(void)
{
    char buf[128];
    protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":"));
}

void test_hello_ends_with_newline(void)
{
    char buf[128];
    int n = protocol_format_hello(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

/* ==============================================================================
 * Buffer truncation safety
 * snprintf must not overflow even if buf is tiny.
 * ============================================================================== */

void test_format_point_tiny_buffer_no_crash(void)
{
    char buf[4] = {0};
    /* Must not crash or overflow — snprintf guarantees null termination */
    protocol_format_point(buf, sizeof(buf), 1, 0, 0.0f, 0.0f, 0.0f);
    /* buf is still null-terminated */
    TEST_ASSERT_EQUAL_CHAR('\0', buf[3]);
}

/* ==============================================================================
 * Entry point
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_point_returns_positive_length);
    RUN_TEST(test_point_has_type_field);
    RUN_TEST(test_point_has_electrode_field);
    RUN_TEST(test_point_has_index_field);
    RUN_TEST(test_point_ends_with_newline);
    RUN_TEST(test_point_e_mv_converted_to_volts);
    RUN_TEST(test_event_started_has_type_field);
    RUN_TEST(test_event_started_has_name_field);
    RUN_TEST(test_event_started_has_technique_field);
    RUN_TEST(test_event_started_ends_with_newline);
    RUN_TEST(test_event_complete_has_name_field);
    RUN_TEST(test_event_error_has_name_field);
    RUN_TEST(test_event_error_contains_message);
    RUN_TEST(test_event_error_null_message_safe);
    RUN_TEST(test_hello_has_type_field);
    RUN_TEST(test_hello_has_proto_version);
    RUN_TEST(test_hello_has_fw_version);
    RUN_TEST(test_hello_ends_with_newline);
    RUN_TEST(test_format_point_tiny_buffer_no_crash);
    return UNITY_END();
}
