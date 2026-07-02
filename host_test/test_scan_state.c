/**
 * test_scan_state.c
 * Unity host tests for the pure scan state machine (scan_state_next).
 *
 * Tests every valid transition defined in scan_state.c, every invalid-transition
 * no-op, and the three complete scan lifecycle sequences (normal / aborted / error).
 * These run on the host PC via ctest — no board, no FreeRTOS, no IDF required.
 *
 * State-transition table under test:
 *
 *   IDLE          + START        → EQUILIBRATING
 *   EQUILIBRATING + EQUILIB_DONE → RUNNING
 *   EQUILIBRATING + ABORTED      → ABORTING
 *   EQUILIBRATING + ERROR        → ERROR
 *   RUNNING       + SCAN_DONE    → COMPLETE
 *   RUNNING       + ABORTED      → ABORTING
 *   RUNNING       + ERROR        → ERROR
 *   COMPLETE      + RESET        → IDLE
 *   ABORTING      + RESET        → IDLE
 *   ERROR         + RESET        → IDLE
 *   (all other (state, event) pairs) → unchanged (no-op)
 */

#include "unity.h"
#include "echem_core/scan_state.h"

void setUp(void)    {}
void tearDown(void) {}

/* ==============================================================================
 * Single-step transitions — valid
 * ============================================================================== */

void test_idle_start_gives_equilibrating(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_EQUILIBRATING,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_START));
}

void test_equilibrating_equilib_done_gives_running(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING,
        scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_EQUILIB_DONE));
}

void test_equilibrating_aborted_gives_aborting(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING,
        scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_ABORTED));
}

void test_equilibrating_error_gives_error(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR,
        scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_ERROR));
}

void test_running_scan_done_gives_complete(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_SCAN_DONE));
}

void test_running_aborted_gives_aborting(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_ABORTED));
}

void test_running_error_gives_error(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_ERROR));
}

void test_complete_reset_gives_idle(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_RESET));
}

void test_aborting_reset_gives_idle(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_ABORTING, SCAN_EVT_RESET));
}

void test_error_reset_gives_idle(void)
{
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_RESET));
}

/* ==============================================================================
 * Invalid transitions — no-op (state unchanged)
 * ============================================================================== */

void test_idle_invalid_events_noop(void)
{
    /* Every event except START is a no-op from IDLE. */
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_EQUILIB_DONE));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_SCAN_DONE));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_ABORTED));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_ERROR));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE,
        scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_RESET));
}

void test_running_invalid_events_noop(void)
{
    /* START and EQUILIB_DONE are invalid from RUNNING. */
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_START));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_EQUILIB_DONE));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING,
        scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_RESET));
}

void test_complete_invalid_events_noop(void)
{
    /* Only RESET is valid from COMPLETE. */
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE,
        scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_START));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE,
        scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_SCAN_DONE));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE,
        scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_ABORTED));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE,
        scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_ERROR));
}

void test_aborting_invalid_events_noop(void)
{
    /* Only RESET is valid from ABORTING. */
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING,
        scan_state_next(SCAN_STATE_ABORTING, SCAN_EVT_START));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING,
        scan_state_next(SCAN_STATE_ABORTING, SCAN_EVT_SCAN_DONE));
}

void test_error_invalid_events_noop(void)
{
    /* Only RESET is valid from ERROR. */
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR,
        scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_START));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR,
        scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_SCAN_DONE));
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR,
        scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_ABORTED));
}

/* ==============================================================================
 * Full lifecycle sequences
 * ============================================================================== */

void test_normal_scan_sequence(void)
{
    /* IDLE → EQUILIBRATING → RUNNING → COMPLETE → IDLE */
    scan_state_t s = SCAN_STATE_IDLE;

    s = scan_state_next(s, SCAN_EVT_START);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_EQUILIBRATING, s);

    s = scan_state_next(s, SCAN_EVT_EQUILIB_DONE);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING, s);

    s = scan_state_next(s, SCAN_EVT_SCAN_DONE);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_COMPLETE, s);

    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);
}

void test_aborted_during_running_sequence(void)
{
    /* IDLE → EQUILIBRATING → RUNNING → ABORTING → IDLE */
    scan_state_t s = SCAN_STATE_IDLE;

    s = scan_state_next(s, SCAN_EVT_START);
    s = scan_state_next(s, SCAN_EVT_EQUILIB_DONE);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING, s);

    s = scan_state_next(s, SCAN_EVT_ABORTED);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING, s);

    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);
}

void test_aborted_during_equilibrating_sequence(void)
{
    /* IDLE → EQUILIBRATING → ABORTING → IDLE */
    scan_state_t s = SCAN_STATE_IDLE;

    s = scan_state_next(s, SCAN_EVT_START);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_EQUILIBRATING, s);

    s = scan_state_next(s, SCAN_EVT_ABORTED);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ABORTING, s);

    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);
}

void test_error_during_running_sequence(void)
{
    /* IDLE → EQUILIBRATING → RUNNING → ERROR → IDLE */
    scan_state_t s = SCAN_STATE_IDLE;

    s = scan_state_next(s, SCAN_EVT_START);
    s = scan_state_next(s, SCAN_EVT_EQUILIB_DONE);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_RUNNING, s);

    s = scan_state_next(s, SCAN_EVT_ERROR);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_ERROR, s);

    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);
}

void test_two_consecutive_scans(void)
{
    /* First scan: normal. Second scan: aborted. State must be IDLE between them. */
    scan_state_t s = SCAN_STATE_IDLE;

    /* Scan 1 */
    s = scan_state_next(s, SCAN_EVT_START);
    s = scan_state_next(s, SCAN_EVT_EQUILIB_DONE);
    s = scan_state_next(s, SCAN_EVT_SCAN_DONE);
    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);

    /* Scan 2 */
    s = scan_state_next(s, SCAN_EVT_START);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_EQUILIBRATING, s); /* Can start again */
    s = scan_state_next(s, SCAN_EVT_EQUILIB_DONE);
    s = scan_state_next(s, SCAN_EVT_ABORTED);
    s = scan_state_next(s, SCAN_EVT_RESET);
    TEST_ASSERT_EQUAL_INT(SCAN_STATE_IDLE, s);
}

/* ==============================================================================
 * Entry point
 * ============================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* Valid single-step transitions */
    RUN_TEST(test_idle_start_gives_equilibrating);
    RUN_TEST(test_equilibrating_equilib_done_gives_running);
    RUN_TEST(test_equilibrating_aborted_gives_aborting);
    RUN_TEST(test_equilibrating_error_gives_error);
    RUN_TEST(test_running_scan_done_gives_complete);
    RUN_TEST(test_running_aborted_gives_aborting);
    RUN_TEST(test_running_error_gives_error);
    RUN_TEST(test_complete_reset_gives_idle);
    RUN_TEST(test_aborting_reset_gives_idle);
    RUN_TEST(test_error_reset_gives_idle);

    /* Invalid transitions (no-op) */
    RUN_TEST(test_idle_invalid_events_noop);
    RUN_TEST(test_running_invalid_events_noop);
    RUN_TEST(test_complete_invalid_events_noop);
    RUN_TEST(test_aborting_invalid_events_noop);
    RUN_TEST(test_error_invalid_events_noop);

    /* Full lifecycle sequences */
    RUN_TEST(test_normal_scan_sequence);
    RUN_TEST(test_aborted_during_running_sequence);
    RUN_TEST(test_aborted_during_equilibrating_sequence);
    RUN_TEST(test_error_during_running_sequence);
    RUN_TEST(test_two_consecutive_scans);

    return UNITY_END();
}
