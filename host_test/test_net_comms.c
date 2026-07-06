/**
 * test_net_comms.c
 * Unity host tests for the P5 net_comms wire-protocol contract.
 *
 * What is tested here (pure C, no IDF, no hardware, runs in CI):
 *
 *   WS binary frame
 *     - sizeof(net_ws_dp_frame_t) == NET_WS_FRAME_SIZE (16 bytes)
 *     - All field offsets match the documented layout
 *     - Encoding a known DataPoint produces the expected byte sequence
 *     - frame_type byte == NET_WS_FRAME_TYPE_DATAPOINT (0x01)
 *
 *   MIME type detection  (net_content_type)
 *     - Each supported extension maps to the correct MIME string
 *     - Unknown extension returns "text/plain"
 *     - Exact-suffix matching: "style.css.bak" does NOT match .css
 *     - Case-sensitivity (extensions are lowercase on LittleFS)
 *
 *   Path traversal guard  (net_path_is_traversal)
 *     - Clean paths return 0
 *     - Paths containing ".." return 1
 *     - Edge cases: "/" ".." "/a/b/../c" "endpoint.." "..html"
 *
 *   ends_with helper  (net_ends_with)
 *     - Basic true/false cases
 *     - Empty string edge cases
 *     - Suffix longer than string
 *
 *   CSV wire format
 *     - NET_CSV_HEADER contains all required column names
 *     - NET_CSV_HEADER ends with \r\n (CRLF, RFC 4180)
 *     - NET_CSV_ROW_FMT produces correct output for known values
 *
 *   Protocol constants
 *     - NET_PROTOCOL_VERSION == 1
 *     - Event name strings are non-empty and match documented values
 *     - Command name strings are non-empty and match documented values
 */

#include "unity.h"
#include "net_comms_protocol.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/* ============================================================================
 * Helpers
 * ========================================================================== */

/** Bit-cast float32 to uint32 for byte-level comparison. */
static uint32_t f32_to_u32(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

/* ============================================================================
 * WS binary frame — struct size
 * ========================================================================== */

void test_frame_size_is_16_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT(NET_WS_FRAME_SIZE, sizeof(net_ws_dp_frame_t));
}

void test_frame_size_constant_matches_sizeof(void)
{
    TEST_ASSERT_EQUAL_UINT(16u, NET_WS_FRAME_SIZE);
}

/* ============================================================================
 * WS binary frame — field offsets
 * ========================================================================== */

void test_frame_offset_frame_type_is_0(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, offsetof(net_ws_dp_frame_t, frame_type));
}

void test_frame_offset_electrode_is_1(void)
{
    TEST_ASSERT_EQUAL_UINT(1u, offsetof(net_ws_dp_frame_t, electrode));
}

void test_frame_offset_idx_is_2(void)
{
    TEST_ASSERT_EQUAL_UINT(2u, offsetof(net_ws_dp_frame_t, idx));
}

void test_frame_offset_E_mV_is_4(void)
{
    TEST_ASSERT_EQUAL_UINT(4u, offsetof(net_ws_dp_frame_t, E_mV));
}

void test_frame_offset_I_uA_is_8(void)
{
    TEST_ASSERT_EQUAL_UINT(8u, offsetof(net_ws_dp_frame_t, I_uA));
}

void test_frame_offset_RE_mV_is_12(void)
{
    TEST_ASSERT_EQUAL_UINT(12u, offsetof(net_ws_dp_frame_t, RE_mV));
}

/* ============================================================================
 * WS binary frame — encoding known values
 * ========================================================================== */

void test_frame_type_byte_is_0x01(void)
{
    net_ws_dp_frame_t f = {
        .frame_type = NET_WS_FRAME_TYPE_DATAPOINT,
        .electrode  = 1,
        .idx        = 0,
        .E_mV       = 0.0f,
        .I_uA       = 0.0f,
        .RE_mV      = 0.0f,
    };
    const uint8_t *b = (const uint8_t *)&f;
    TEST_ASSERT_EQUAL_UINT8(0x01u, b[0]);
}

void test_frame_electrode_field_encodes_correctly(void)
{
    net_ws_dp_frame_t f = { .frame_type = 0x01, .electrode = 3 };
    const uint8_t *b = (const uint8_t *)&f;
    TEST_ASSERT_EQUAL_UINT8(3u, b[1]);
}

void test_frame_idx_little_endian(void)
{
    net_ws_dp_frame_t f = {0};
    f.idx = 0x0102u; /* LSB=0x02, MSB=0x01 in little-endian */
    const uint8_t *b = (const uint8_t *)&f;
    TEST_ASSERT_EQUAL_UINT8(0x02u, b[2]); /* low byte at offset 2 */
    TEST_ASSERT_EQUAL_UINT8(0x01u, b[3]); /* high byte at offset 3 */
}

void test_frame_E_mV_encodes_known_float(void)
{
    const float E = -500.0f;
    net_ws_dp_frame_t f = {0};
    f.E_mV = E;
    uint32_t from_frame;
    memcpy(&from_frame, (const uint8_t *)&f + 4, 4);
    TEST_ASSERT_EQUAL_UINT32(f32_to_u32(E), from_frame);
}

void test_frame_I_uA_encodes_known_float(void)
{
    const float I = 12.345f;
    net_ws_dp_frame_t f = {0};
    f.I_uA = I;
    uint32_t from_frame;
    memcpy(&from_frame, (const uint8_t *)&f + 8, 4);
    TEST_ASSERT_EQUAL_UINT32(f32_to_u32(I), from_frame);
}

void test_frame_RE_mV_encodes_known_float(void)
{
    const float RE = -495.5f;
    net_ws_dp_frame_t f = {0};
    f.RE_mV = RE;
    uint32_t from_frame;
    memcpy(&from_frame, (const uint8_t *)&f + 12, 4);
    TEST_ASSERT_EQUAL_UINT32(f32_to_u32(RE), from_frame);
}

void test_frame_full_encode_decode_roundtrip(void)
{
    net_ws_dp_frame_t orig = {
        .frame_type = NET_WS_FRAME_TYPE_DATAPOINT,
        .electrode  = 2,
        .idx        = 42,
        .E_mV       = -250.0f,
        .I_uA       =   8.5f,
        .RE_mV      = -248.0f,
    };

    /* Simulate a network round-trip: copy bytes, then re-read struct */
    uint8_t wire[NET_WS_FRAME_SIZE];
    memcpy(wire, &orig, NET_WS_FRAME_SIZE);

    net_ws_dp_frame_t decoded;
    memcpy(&decoded, wire, NET_WS_FRAME_SIZE);

    TEST_ASSERT_EQUAL_UINT8(NET_WS_FRAME_TYPE_DATAPOINT, decoded.frame_type);
    TEST_ASSERT_EQUAL_UINT8(2,   decoded.electrode);
    TEST_ASSERT_EQUAL_UINT16(42, decoded.idx);
    TEST_ASSERT_EQUAL_FLOAT(-250.0f, decoded.E_mV);
    TEST_ASSERT_EQUAL_FLOAT(  8.5f,  decoded.I_uA);
    TEST_ASSERT_EQUAL_FLOAT(-248.0f, decoded.RE_mV);
}

/* ============================================================================
 * MIME type detection  (net_content_type)
 * ========================================================================== */

void test_mime_html(void)
{
    TEST_ASSERT_EQUAL_STRING("text/html", net_content_type("index.html"));
}

void test_mime_css(void)
{
    TEST_ASSERT_EQUAL_STRING("text/css", net_content_type("style.css"));
}

void test_mime_js(void)
{
    TEST_ASSERT_EQUAL_STRING("application/javascript", net_content_type("app.js"));
}

void test_mime_json(void)
{
    TEST_ASSERT_EQUAL_STRING("application/json", net_content_type("data.json"));
}

void test_mime_png(void)
{
    TEST_ASSERT_EQUAL_STRING("image/png", net_content_type("logo.png"));
}

void test_mime_svg(void)
{
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", net_content_type("icon.svg"));
}

void test_mime_ico(void)
{
    TEST_ASSERT_EQUAL_STRING("image/x-icon", net_content_type("favicon.ico"));
}

void test_mime_csv(void)
{
    TEST_ASSERT_EQUAL_STRING("text/csv", net_content_type("scan.csv"));
}

void test_mime_gz(void)
{
    TEST_ASSERT_EQUAL_STRING("application/gzip", net_content_type("app.js.gz"));
}

void test_mime_unknown_returns_text_plain(void)
{
    TEST_ASSERT_EQUAL_STRING("text/plain", net_content_type("file.xyz"));
}

void test_mime_no_extension_returns_text_plain(void)
{
    TEST_ASSERT_EQUAL_STRING("text/plain", net_content_type("README"));
}

/* Bug-5 regression: suffix match must be exact, not substring */
void test_mime_no_false_positive_on_css_bak(void)
{
    /* "style.css.bak" should NOT match .css — ends with .bak */
    TEST_ASSERT_EQUAL_STRING("text/plain", net_content_type("style.css.bak"));
}

void test_mime_no_false_positive_on_html_in_dir(void)
{
    /* "/html/app.js" should NOT match .html — ends with .js */
    TEST_ASSERT_EQUAL_STRING("application/javascript", net_content_type("/html/app.js"));
}

void test_mime_path_with_directory(void)
{
    TEST_ASSERT_EQUAL_STRING("text/html", net_content_type("/www/index.html"));
}

/* ============================================================================
 * ends_with helper  (net_ends_with)
 * ========================================================================== */

void test_ends_with_true_simple(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_ends_with("hello.html", ".html"));
}

void test_ends_with_false_different_suffix(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_ends_with("hello.html", ".css"));
}

void test_ends_with_exact_match(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_ends_with(".html", ".html"));
}

void test_ends_with_suffix_longer_than_string(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_ends_with("a", ".html"));
}

void test_ends_with_empty_string(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_ends_with("", ".html"));
}

void test_ends_with_empty_suffix(void)
{
    /* Any string ends with empty suffix */
    TEST_ASSERT_EQUAL_INT(1, net_ends_with("anything", ""));
}

void test_ends_with_both_empty(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_ends_with("", ""));
}

/* ============================================================================
 * Path traversal guard  (net_path_is_traversal)
 * ========================================================================== */

void test_traversal_clean_root(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_path_is_traversal("/"));
}

void test_traversal_clean_file(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_path_is_traversal("/index.html"));
}

void test_traversal_clean_nested(void)
{
    TEST_ASSERT_EQUAL_INT(0, net_path_is_traversal("/assets/style.css"));
}

void test_traversal_dotdot_detected(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_path_is_traversal("/../etc/passwd"));
}

void test_traversal_double_dot_mid_path(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_path_is_traversal("/a/b/../c"));
}

void test_traversal_double_dot_alone(void)
{
    TEST_ASSERT_EQUAL_INT(1, net_path_is_traversal(".."));
}

void test_traversal_single_dot_clean(void)
{
    /* Single dot is not a traversal */
    TEST_ASSERT_EQUAL_INT(0, net_path_is_traversal("/a/./b"));
}

void test_traversal_dotdot_in_filename(void)
{
    /* "endpoint.." — ends with ".." → traversal */
    TEST_ASSERT_EQUAL_INT(1, net_path_is_traversal("/endpoint.."));
}

void test_traversal_dotdot_prefix_in_filename(void)
{
    /* "..html" — starts with ".." → traversal */
    TEST_ASSERT_EQUAL_INT(1, net_path_is_traversal("/..html"));
}

/* ============================================================================
 * CSV wire format constants
 * ========================================================================== */

void test_csv_header_contains_electrode_column(void)
{
    TEST_ASSERT_NOT_NULL(strstr(NET_CSV_HEADER, "electrode"));
}

void test_csv_header_contains_idx_column(void)
{
    TEST_ASSERT_NOT_NULL(strstr(NET_CSV_HEADER, "idx"));
}

void test_csv_header_contains_E_mV_column(void)
{
    TEST_ASSERT_NOT_NULL(strstr(NET_CSV_HEADER, "E_mV"));
}

void test_csv_header_contains_I_uA_column(void)
{
    TEST_ASSERT_NOT_NULL(strstr(NET_CSV_HEADER, "I_uA"));
}

void test_csv_header_contains_RE_mV_column(void)
{
    TEST_ASSERT_NOT_NULL(strstr(NET_CSV_HEADER, "RE_mV"));
}

void test_csv_header_ends_with_crlf(void)
{
    const char *h = NET_CSV_HEADER;
    size_t len = strlen(h);
    TEST_ASSERT_GREATER_THAN(1u, len);
    TEST_ASSERT_EQUAL_CHAR('\n', h[len - 1]);
    TEST_ASSERT_EQUAL_CHAR('\r', h[len - 2]);
}

void test_csv_row_format_produces_correct_output(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), NET_CSV_ROW_FMT,
                     (unsigned)1, (unsigned)5,
                     (double)-500.0f, (double)12.3456f, (double)-495.0f);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "1,5,-500.0000,12.3456,-495.0000"));
    /* Must end with CRLF */
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
    TEST_ASSERT_EQUAL_CHAR('\r', buf[n - 2]);
}

/* ============================================================================
 * Protocol constants
 * ========================================================================== */

void test_protocol_version_is_1(void)
{
    TEST_ASSERT_EQUAL_INT(1, NET_PROTOCOL_VERSION);
}

void test_fw_version_string_nonempty(void)
{
    TEST_ASSERT_GREATER_THAN(0u, strlen(NET_FW_VERSION_STR));
}

void test_event_name_started_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("scan_started", NET_EVT_NAME_STARTED);
}

void test_event_name_complete_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("scan_complete", NET_EVT_NAME_COMPLETE);
}

void test_event_name_aborted_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("scan_aborted", NET_EVT_NAME_ABORTED);
}

void test_event_name_error_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("scan_error", NET_EVT_NAME_ERROR);
}

void test_cmd_start_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("start", NET_CMD_START);
}

void test_cmd_abort_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("abort", NET_CMD_ABORT);
}

void test_cmd_state_correct(void)
{
    TEST_ASSERT_EQUAL_STRING("state", NET_CMD_STATE);
}

void test_ws_path_starts_with_slash(void)
{
    TEST_ASSERT_EQUAL_CHAR('/', NET_WS_PATH[0]);
}

/* ============================================================================
 * main
 * ========================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* WS frame size */
    RUN_TEST(test_frame_size_is_16_bytes);
    RUN_TEST(test_frame_size_constant_matches_sizeof);

    /* WS frame offsets */
    RUN_TEST(test_frame_offset_frame_type_is_0);
    RUN_TEST(test_frame_offset_electrode_is_1);
    RUN_TEST(test_frame_offset_idx_is_2);
    RUN_TEST(test_frame_offset_E_mV_is_4);
    RUN_TEST(test_frame_offset_I_uA_is_8);
    RUN_TEST(test_frame_offset_RE_mV_is_12);

    /* WS frame encoding */
    RUN_TEST(test_frame_type_byte_is_0x01);
    RUN_TEST(test_frame_electrode_field_encodes_correctly);
    RUN_TEST(test_frame_idx_little_endian);
    RUN_TEST(test_frame_E_mV_encodes_known_float);
    RUN_TEST(test_frame_I_uA_encodes_known_float);
    RUN_TEST(test_frame_RE_mV_encodes_known_float);
    RUN_TEST(test_frame_full_encode_decode_roundtrip);

    /* MIME type detection */
    RUN_TEST(test_mime_html);
    RUN_TEST(test_mime_css);
    RUN_TEST(test_mime_js);
    RUN_TEST(test_mime_json);
    RUN_TEST(test_mime_png);
    RUN_TEST(test_mime_svg);
    RUN_TEST(test_mime_ico);
    RUN_TEST(test_mime_csv);
    RUN_TEST(test_mime_gz);
    RUN_TEST(test_mime_unknown_returns_text_plain);
    RUN_TEST(test_mime_no_extension_returns_text_plain);
    RUN_TEST(test_mime_no_false_positive_on_css_bak);
    RUN_TEST(test_mime_no_false_positive_on_html_in_dir);
    RUN_TEST(test_mime_path_with_directory);

    /* ends_with */
    RUN_TEST(test_ends_with_true_simple);
    RUN_TEST(test_ends_with_false_different_suffix);
    RUN_TEST(test_ends_with_exact_match);
    RUN_TEST(test_ends_with_suffix_longer_than_string);
    RUN_TEST(test_ends_with_empty_string);
    RUN_TEST(test_ends_with_empty_suffix);
    RUN_TEST(test_ends_with_both_empty);

    /* Path traversal guard */
    RUN_TEST(test_traversal_clean_root);
    RUN_TEST(test_traversal_clean_file);
    RUN_TEST(test_traversal_clean_nested);
    RUN_TEST(test_traversal_dotdot_detected);
    RUN_TEST(test_traversal_double_dot_mid_path);
    RUN_TEST(test_traversal_double_dot_alone);
    RUN_TEST(test_traversal_single_dot_clean);
    RUN_TEST(test_traversal_dotdot_in_filename);
    RUN_TEST(test_traversal_dotdot_prefix_in_filename);

    /* CSV format */
    RUN_TEST(test_csv_header_contains_electrode_column);
    RUN_TEST(test_csv_header_contains_idx_column);
    RUN_TEST(test_csv_header_contains_E_mV_column);
    RUN_TEST(test_csv_header_contains_I_uA_column);
    RUN_TEST(test_csv_header_contains_RE_mV_column);
    RUN_TEST(test_csv_header_ends_with_crlf);
    RUN_TEST(test_csv_row_format_produces_correct_output);

    /* Protocol constants */
    RUN_TEST(test_protocol_version_is_1);
    RUN_TEST(test_fw_version_string_nonempty);
    RUN_TEST(test_event_name_started_correct);
    RUN_TEST(test_event_name_complete_correct);
    RUN_TEST(test_event_name_aborted_correct);
    RUN_TEST(test_event_name_error_correct);
    RUN_TEST(test_cmd_start_correct);
    RUN_TEST(test_cmd_abort_correct);
    RUN_TEST(test_cmd_state_correct);
    RUN_TEST(test_ws_path_starts_with_slash);

    return UNITY_END();
}
