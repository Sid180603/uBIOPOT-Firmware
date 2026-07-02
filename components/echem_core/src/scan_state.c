#include "echem_core/scan_state.h"

/**
 * scan_state_next — deterministic scan state-transition table.
 *
 * Layout: for each current state, only the explicitly-listed events cause
 * a transition; all other events return the current state unchanged.
 */
scan_state_t scan_state_next(scan_state_t current, scan_event_t event)
{
    switch (current) {

    case SCAN_STATE_IDLE:
        if (event == SCAN_EVT_START)       return SCAN_STATE_EQUILIBRATING;
        break;

    case SCAN_STATE_EQUILIBRATING:
        if (event == SCAN_EVT_EQUILIB_DONE) return SCAN_STATE_RUNNING;
        if (event == SCAN_EVT_ABORTED)      return SCAN_STATE_ABORTING;
        if (event == SCAN_EVT_ERROR)        return SCAN_STATE_ERROR;
        break;

    case SCAN_STATE_RUNNING:
        if (event == SCAN_EVT_SCAN_DONE)    return SCAN_STATE_COMPLETE;
        if (event == SCAN_EVT_ABORTED)      return SCAN_STATE_ABORTING;
        if (event == SCAN_EVT_ERROR)        return SCAN_STATE_ERROR;
        break;

    case SCAN_STATE_COMPLETE:
        if (event == SCAN_EVT_RESET)        return SCAN_STATE_IDLE;
        break;

    case SCAN_STATE_ABORTING:
        if (event == SCAN_EVT_RESET)        return SCAN_STATE_IDLE;
        break;

    case SCAN_STATE_ERROR:
        if (event == SCAN_EVT_RESET)        return SCAN_STATE_IDLE;
        break;

    default:
        break;
    }

    return current; /* Invalid (state, event) pair — stay put. */
}
