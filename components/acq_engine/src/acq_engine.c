/**
 * @file acq_engine.c
 * @brief Acquisition engine — FreeRTOS task infrastructure.
 *
 * Two-task architecture (P3 plan, verified vs ESP-IDF SMP FreeRTOS docs):
 *
 *   AcquisitionTask (Core 1, highest priority)
 *     Sole owner of pstat_hal.  Blocks on the command queue.  On CMD_START:
 *     selects electrode, runs technique->run() with DI callbacks, emits
 *     DataPoints/events to the dispatch queue (fast, non-blocking xQueueSend).
 *
 *   DispatcherTask (Core 0, medium priority)
 *     Drains the dispatch queue.  DataPoints → append scan buffer (mutex) +
 *     fan-out to sinks.  Events → fan-out to sinks.
 *
 * Why two tasks: the acquisition loop (Core 1) must NEVER block on WiFi/httpd.
 * By handing data to a queue and letting Core-0 do fan-out, Core-1 timing stays
 * metrologically clean even if a WebSocket client stalls.  This property is
 * proven by the "slow-sink does not perturb Core-1" test (P3 DoD item 4).
 *
 * Overflow policy: emit_point uses a non-blocking xQueueSend (timeout = 0).
 * If the dispatch queue is full, the point is dropped with a warning.  Timing
 * integrity on Core 1 is preserved; the server buffer gets every point because
 * the Dispatcher appends to it before calling any slow sink.
 * NOTE: at DPV rates (~5–10 pts/s), queue overflow should never occur in practice.
 *
 * FPU caveat (ESP-IDF SMP FreeRTOS): tasks that use float are auto-pinned to
 * their creation core.  All float math lives in AcquisitionTask (Core 1) and
 * never in ISRs — this is correct by construction.
 */

#include "acq_engine.h"

#include "pstat_hal/pstat_hal.h"
#include "echem_core/technique.h"
#include "echem_core/dpv.h"
#include "echem_core/cv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "acq_engine";

/* ==========================================================================
 * Internal message type for the unified dispatch queue.
 *
 * A single queue carries both DataPoints and Events so the Dispatcher can
 * process them in the order they were produced by AcquisitionTask.
 * ========================================================================== */

typedef enum { DMSG_DATAPOINT, DMSG_EVENT } dmsg_type_t;

typedef struct {
    dmsg_type_t type;
    union {
        DataPoint pt;
        struct {
            scan_event_t evt;
            char         info[48];
        } ev;
    };
} dmsg_t;

/* ==========================================================================
 * Command queue message (AcquisitionTask inbox)
 * ========================================================================== */

typedef enum { ECMD_START, ECMD_ZERO } ecmd_type_t;

/** Max size of any technique's params struct carried in a command. */
#define ENGINE_PARAMS_MAX 64

typedef struct {
    ecmd_type_t type;
    uint8_t     electrode;                  /**< 0 = ALL sequential, 1/2/3 = specific. */
    char        technique[8];               /**< Technique name, e.g. "DPV" / "CV". */
    uint8_t     params[ENGINE_PARAMS_MAX];  /**< Technique params struct bytes. */
} engine_cmd_t;

/* ==========================================================================
 * Module-level static state
 * ========================================================================== */

static pstat_calib_t       *s_cal;

static QueueHandle_t        s_cmd_queue;     /**< engine_cmd_t — engine inbox. */
static QueueHandle_t        s_dp_queue;      /**< dmsg_t — datapoints + events. */

static SemaphoreHandle_t    s_buf_mutex;     /**< Guards s_scan_buf + s_scan_buf_count. */
static SemaphoreHandle_t    s_sink_mutex;    /**< Guards s_sinks[] + s_sink_count. */

/**
 * Scan state: written by AcquisitionTask (Core 1), read atomically by any caller.
 * Using _Atomic ensures lock-free read on 32-bit ARM (aligned 32-bit stores are
 * naturally atomic, but _Atomic makes the intent explicit and portable).
 */
static _Atomic scan_state_t  s_scan_state;
static _Atomic bool          s_abort_flag;

/* Server-authoritative scan buffer. */
static DataPoint    s_scan_buf[ENGINE_SCAN_BUF_MAX];
static uint16_t     s_scan_buf_count;

/* Sink registry. */
static engine_sink_t s_sinks[ENGINE_MAX_SINKS];
static int           s_sink_count;

/* Per-scan tracking — only written by AcquisitionTask (Core 1). */
static uint8_t  s_current_electrode;
static uint16_t s_point_idx;
static bool     s_equilib_done_posted; /* true after first point fires EQUILIB_DONE */

/* ==========================================================================
 * Internal helpers
 * ========================================================================== */

/** Update the atomic state variable (called only from AcquisitionTask). */
static inline void state_set(scan_state_t new_state)
{
    atomic_store(&s_scan_state, new_state);
}

/**
 * Post an event message to the dispatch queue.
 * Non-blocking with a short timeout so AcquisitionTask is never stalled by a
 * full queue (which would imply Dispatcher is not keeping up — a bug, not a
 * design scenario at DPV rates).
 */
static void post_event(scan_event_t evt, const char *info)
{
    dmsg_t msg;
    msg.type    = DMSG_EVENT;
    msg.ev.evt  = evt;
    if (info) {
        strncpy(msg.ev.info, info, sizeof(msg.ev.info) - 1);
        msg.ev.info[sizeof(msg.ev.info) - 1] = '\0';
    } else {
        msg.ev.info[0] = '\0';
    }
    if (xQueueSend(s_dp_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full — dropping event %d", (int)evt);
    }
}

/** Fan-out a DataPoint to all registered sinks. Called from DispatcherTask. */
static void fan_out_point(const DataPoint *pt)
{
    if (xSemaphoreTake(s_sink_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    for (int i = 0; i < s_sink_count; i++) {
        if (s_sinks[i].on_point) {
            s_sinks[i].on_point(pt, s_sinks[i].ctx);
        }
    }
    xSemaphoreGive(s_sink_mutex);
}

/** Fan-out an event to all registered sinks. Called from DispatcherTask. */
static void fan_out_event(scan_event_t evt, const char *info)
{
    if (xSemaphoreTake(s_sink_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    for (int i = 0; i < s_sink_count; i++) {
        if (s_sinks[i].on_event) {
            s_sinks[i].on_event(evt, info[0] ? info : NULL, s_sinks[i].ctx);
        }
    }
    xSemaphoreGive(s_sink_mutex);
}

/* ==========================================================================
 * DI callbacks — wired by AcquisitionTask into hal_callbacks_t.
 * All float math lives here (Core 1); never called from an ISR.
 * ========================================================================== */

static void eng_set_voltage(float cell_v, void *ctx)
{
    (void)ctx;
    pstat_dac_set_volt(cell_v, s_cal);
}

static float eng_read_current_uA(uint8_t n_avg, void *ctx)
{
    (void)ctx;
    return pstat_adc_read_current_uA(n_avg, s_cal);
}

static float eng_read_cell_volt(void *ctx)
{
    (void)ctx;
    return pstat_adc_read_cell_volt(s_cal);
}

static void eng_delay_ms(uint32_t ms, void *ctx)
{
    (void)ctx;
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool eng_check_abort(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_abort_flag);
}

/**
 * emit_point — called by technique->run() for every completed DPV step.
 *
 * Fast path: constructs a DataPoint and posts it to the dispatch queue with a
 * timeout of 0 (non-blocking).  If the queue is full, the point is dropped.
 * The server buffer still gets every point because the Dispatcher appends there
 * first, before calling any slow sink (see dispatcher_task).
 *
 * NOTE: s_current_electrode and s_point_idx are only written by AcquisitionTask
 * (Core 1), so there is no race.
 */
static void eng_emit_point(float E_mV, float I_uA, float RE_mV, void *ctx)
{
    (void)ctx;

    /* On the very first data point of a scan, transition EQUILIBRATING → RUNNING
     * and notify sinks.  This fires only after dpv_run() has completed its
     * equilibration loop, so the EQUILIB_DONE event reaches sinks at the true
     * end of physical equilibration — not prematurely before it starts. */
    if (!s_equilib_done_posted) {
        s_equilib_done_posted = true;
        state_set(scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_EQUILIB_DONE));
        post_event(SCAN_EVT_EQUILIB_DONE, NULL);
    }

    DataPoint pt = {
        .electrode = s_current_electrode,
        .idx       = s_point_idx++,
        .E_mV      = E_mV,
        .I_uA      = I_uA,
        .RE_mV     = RE_mV,
    };
    dmsg_t msg;
    msg.type = DMSG_DATAPOINT;
    msg.pt   = pt;
    if (xQueueSend(s_dp_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "dp queue full — dropping point (E=%.1f mV, idx=%u)",
                 (double)E_mV, (unsigned)pt.idx);
    }
}

/* Static hal_callbacks_t struct wired to the DI callbacks above. */
static const hal_callbacks_t s_hal_cb = {
    .set_voltage     = eng_set_voltage,
    .read_current_uA = eng_read_current_uA,
    .read_cell_volt  = eng_read_cell_volt,
    .emit_point      = eng_emit_point,
    .check_abort     = eng_check_abort,
    .delay_ms        = eng_delay_ms,
    .ctx             = NULL,
};

/* ==========================================================================
 * DispatcherTask — Core 0, medium priority.
 *
 * Drains the dispatch queue.  For DataPoints: appends to the server buffer
 * (under mutex) FIRST, then fans out to sinks (so a slow sink never causes a
 * dropped point in the buffer).  For Events: fans out directly to sinks.
 * ========================================================================== */

static void dispatcher_task(void *arg)
{
    (void)arg;
    dmsg_t msg;

    for (;;) {
        if (xQueueReceive(s_dp_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.type == DMSG_DATAPOINT) {
            /* Append to server buffer under mutex. */
            if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (s_scan_buf_count < ENGINE_SCAN_BUF_MAX) {
                    s_scan_buf[s_scan_buf_count++] = msg.pt;
                } else {
                    ESP_LOGW(TAG, "scan buffer full — dropping point idx=%u",
                             (unsigned)msg.pt.idx);
                }
                xSemaphoreGive(s_buf_mutex);
            }
            /* Fan out to sinks after the buffer is safely updated. */
            fan_out_point(&msg.pt);

        } else { /* DMSG_EVENT */
            fan_out_event(msg.ev.evt, msg.ev.info);
        }
    }
}

/* ==========================================================================
 * AcquisitionTask — Core 1, highest priority.
 *
 * Single owner of all HAL operations.  No other task may call pstat_* while
 * this task is running (enforced by the single-task + DI design).
 * ========================================================================== */

static void acquisition_task(void *arg)
{
    (void)arg;
    engine_cmd_t cmd;

    for (;;) {
        /* Block until a command arrives. */
        xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);

        /* ---- CMD_ZERO ----
         * Auto-zero the current channel (P8): with the working electrode floating
         * (mux fully deselected) the TIA carries no cell current, so its output
         * reflects only the electronics quiescent offset (level-shifter / vref_mid
         * mismatch).  Measure it and store the negated value as current_offset_uA
         * so subsequent readings report ~0 at zero cell current.  This cancels the
         * large constant baseline seen in CV (it already cancels in DPV's
         * differential dI). */
        if (cmd.type == ECMD_ZERO) {
            if (atomic_load(&s_scan_state) != SCAN_STATE_IDLE) {
                ESP_LOGW(TAG, "CMD_ZERO ignored — engine not IDLE (state=%d)",
                         (int)atomic_load(&s_scan_state));
                continue;
            }
            pstat_mux_deselect_all();            /* WE floating — no cell current */
            float prev_offset = s_cal->current_offset_uA;
            s_cal->current_offset_uA = 0.0f;      /* read the raw pedestal */
            pstat_dac_set_volt(0.0f, s_cal);      /* DAC at vref_mid (nominal bias) */
            vTaskDelay(pdMS_TO_TICKS(300));        /* settle TIA / level-shifter */
            float pedestal = pstat_adc_read_current_uA(64, s_cal);
            s_cal->current_offset_uA = -pedestal;  /* cancel it */
            ESP_LOGI(TAG, "CMD_ZERO: current_offset_uA %.2f -> %.2f (pedestal %.2f uA)",
                     prev_offset, s_cal->current_offset_uA, pedestal);
            continue;
        }

        /* ---- CMD_START ---- */
        if (atomic_load(&s_scan_state) != SCAN_STATE_IDLE) {
            ESP_LOGW(TAG, "CMD_START ignored — engine not IDLE (state=%d)",
                     (int)atomic_load(&s_scan_state));
            continue;
        }

        /* Find the requested technique (DPV default, CV, ...). */
        const char *tech_name = (cmd.technique[0] != '\0') ? cmd.technique : "DPV";
        const technique_t *t = technique_find(tech_name);
        if (!t) {
            state_set(scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_ERROR));
            post_event(SCAN_EVT_ERROR, "technique not found");
            state_set(scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_RESET));
            post_event(SCAN_EVT_RESET, NULL);
            continue;
        }

        /* Copy params from command into an aligned union and validate.
         * The union guarantees natural alignment for float/uint32 access on
         * Xtensa (a raw uint8_t[] would not). */
        union {
            dpv_params_t dpv;
            cv_params_t  cv;
            uint8_t      raw[ENGINE_PARAMS_MAX];
        } params;
        size_t psize = t->params_size <= sizeof(params.raw) ? t->params_size : sizeof(params.raw);
        memcpy(&params, cmd.params, psize);
        char err_buf[64] = {0};
        if (t->validate(&params, err_buf, sizeof(err_buf)) != 0) {
            ESP_LOGW(TAG, "%s param validation failed: %s", t->name, err_buf);
            state_set(scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_ERROR));
            post_event(SCAN_EVT_ERROR, err_buf);
            state_set(scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_RESET));
            post_event(SCAN_EVT_RESET, NULL);
            continue;
        }

        /* Determine electrode range. */
        uint8_t elec_first = cmd.electrode, elec_last = cmd.electrode;
        if (cmd.electrode == 0) { elec_first = 1; elec_last = 3; }

        /* Clear server buffer and reset abort flag before the scan. */
        if (xSemaphoreTake(s_buf_mutex, portMAX_DELAY) == pdTRUE) {
            s_scan_buf_count = 0;
            xSemaphoreGive(s_buf_mutex);
        }
        atomic_store(&s_abort_flag, false);

        /* Mirror LEDs: PROCESSING on, READY off. */
        pstat_led_set(PSTAT_LED_PROCESSING, true);
        pstat_led_set(PSTAT_LED_READY, false);

        bool aborted = false, error = false;

        for (uint8_t elec = elec_first; elec <= elec_last; elec++) {

            s_current_electrode   = elec;
            s_point_idx           = 0;
            s_equilib_done_posted = false; /* reset per-electrode; first emit_point fires EQUILIB_DONE */

            /* BUG FIX: for electrodes after the first in an ELECTRODE_ALL scan,
             * the state is RUNNING (from the previous electrode), not IDLE.
             * Transition back through COMPLETE -> IDLE so the START transition
             * fires correctly and sinks see a consistent state sequence. */
            if (elec > elec_first) {
                state_set(scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_SCAN_DONE));
                post_event(SCAN_EVT_SCAN_DONE, NULL);
                state_set(scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_RESET));
                post_event(SCAN_EVT_RESET, NULL);
            }

            /* Select electrode on mux. */
            pstat_mux_select(elec);

            /* State: IDLE -> EQUILIBRATING. Notify sinks — UI shows spinner.
             * EQUILIB_DONE is deferred: eng_emit_point() fires it on the first
             * real data point, after dpv_run() has completed its equilibration
             * delay, so the spinner is visible for the true equilibration period. */
            state_set(scan_state_next(SCAN_STATE_IDLE, SCAN_EVT_START));
            post_event(SCAN_EVT_START, t->name);

            /* Run the technique -- blocks for the full scan duration.
             * The EQUILIBRATING -> RUNNING transition now happens inside
             * eng_emit_point() on the first data point. */
            int rc = t->run(&params, s_cal, &s_hal_cb);

            if (rc > 0 || atomic_load(&s_abort_flag)) {
                aborted = true;
                ESP_LOGI(TAG, "Scan aborted (electrode %u)", (unsigned)elec);
                break;
            } else if (rc < 0) {
                error = true;
                ESP_LOGE(TAG, "Scan error rc=%d (electrode %u)", rc, (unsigned)elec);
                break;
            }
            /* Electrode done -- loop continues to next electrode if ALL. */
        }

        /* Post-scan state transitions.
         * scan_state_next() enforces valid transitions; no ad-hoc state writes.
         *
         * If abort fired during equilibration (before the first emit_point), the
         * engine is still in EQUILIBRATING — transition through that state correctly. */
        if (aborted) {
            scan_state_t cur = atomic_load(&s_scan_state);
            if (cur == SCAN_STATE_EQUILIBRATING) {
                /* Aborted before first data point — skip RUNNING entirely */
                state_set(scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_ABORTED));
            } else {
                state_set(scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_ABORTED));
            }
            post_event(SCAN_EVT_ABORTED, NULL);
            /* Auto-reset to IDLE. */
            state_set(scan_state_next(SCAN_STATE_ABORTING, SCAN_EVT_RESET));
            post_event(SCAN_EVT_RESET, NULL);
        } else if (error) {
            scan_state_t cur = atomic_load(&s_scan_state);
            if (cur == SCAN_STATE_EQUILIBRATING) {
                state_set(scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_ERROR));
            } else {
                state_set(scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_ERROR));
            }
            post_event(SCAN_EVT_ERROR, "scan error");
            /* Auto-reset to IDLE. */
            state_set(scan_state_next(SCAN_STATE_ERROR, SCAN_EVT_RESET));
            post_event(SCAN_EVT_RESET, NULL);
        } else {
            /* All electrodes completed normally.
             * If no points were emitted (degenerate zero-step scan), the state
             * is still EQUILIBRATING — transition through RUNNING first. */
            if (!s_equilib_done_posted) {
                state_set(scan_state_next(SCAN_STATE_EQUILIBRATING, SCAN_EVT_EQUILIB_DONE));
                post_event(SCAN_EVT_EQUILIB_DONE, NULL);
            }
            state_set(scan_state_next(SCAN_STATE_RUNNING, SCAN_EVT_SCAN_DONE));
            post_event(SCAN_EVT_SCAN_DONE, NULL);
            /* Auto-reset to IDLE. */
            state_set(scan_state_next(SCAN_STATE_COMPLETE, SCAN_EVT_RESET));
            post_event(SCAN_EVT_RESET, NULL);
        }

        /* Deselect all electrodes after the scan. */
        pstat_mux_deselect_all();

        /* Mirror LEDs: back to READY. */
        pstat_led_set(PSTAT_LED_PROCESSING, false);
        pstat_led_set(PSTAT_LED_READY, true);
    }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

esp_err_t acq_engine_init(pstat_calib_t *cal)
{
    if (!cal) return ESP_ERR_INVALID_ARG;
    s_cal = cal;

    atomic_store(&s_scan_state, SCAN_STATE_IDLE);
    atomic_store(&s_abort_flag, false);
    s_scan_buf_count = 0;
    s_sink_count     = 0;

    /* Command queue: 4 commands deep (vastly more than needed at DPV rates). */
    s_cmd_queue = xQueueCreate(4, sizeof(engine_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    /* Dispatch queue: 64 messages — far more than one DPV scan can produce
     * between Dispatcher scheduling intervals at DPV rates. */
    s_dp_queue = xQueueCreate(64, sizeof(dmsg_t));
    if (!s_dp_queue) {
        ESP_LOGE(TAG, "failed to create dispatch queue");
        return ESP_ERR_NO_MEM;
    }

    s_buf_mutex = xSemaphoreCreateMutex();
    if (!s_buf_mutex) {
        ESP_LOGE(TAG, "failed to create buf mutex");
        return ESP_ERR_NO_MEM;
    }

    s_sink_mutex = xSemaphoreCreateMutex();
    if (!s_sink_mutex) {
        ESP_LOGE(TAG, "failed to create sink mutex");
        return ESP_ERR_NO_MEM;
    }

    /* AcquisitionTask: Core 1, highest priority, 4 KB stack.
     * Stack needs room for technique->run() local variables + HAL driver calls. */
    if (xTaskCreatePinnedToCore(
            acquisition_task, "acq",
            4096, NULL,
            configMAX_PRIORITIES - 1,
            NULL,
            1 /* Core 1 = APP_CPU, away from WiFi/BT PRO_CPU */
        ) != pdPASS) {
        ESP_LOGE(TAG, "failed to create AcquisitionTask");
        return ESP_FAIL;
    }

    /* DispatcherTask: Core 0, medium priority, 3 KB stack. */
    if (xTaskCreatePinnedToCore(
            dispatcher_task, "disp",
            3072, NULL,
            configMAX_PRIORITIES / 2,
            NULL,
            0 /* Core 0 = PRO_CPU, same as WiFi/BT comms */
        ) != pdPASS) {
        ESP_LOGE(TAG, "failed to create DispatcherTask");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init OK — AcqTask@Core1 + DispTask@Core0");
    return ESP_OK;
}

esp_err_t engine_start(uint8_t electrode, const dpv_params_t *params)
{
    if (!params) return ESP_ERR_INVALID_ARG;
    return engine_start_technique("DPV", electrode, params, sizeof(dpv_params_t));
}

esp_err_t engine_start_technique(const char *technique, uint8_t electrode,
                                 const void *params, size_t params_size)
{
    if (!technique || !params) return ESP_ERR_INVALID_ARG;
    if (params_size > ENGINE_PARAMS_MAX) return ESP_ERR_INVALID_SIZE;
    if (atomic_load(&s_scan_state) != SCAN_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    engine_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type      = ECMD_START;
    cmd.electrode = electrode;
    strncpy(cmd.technique, technique, sizeof(cmd.technique) - 1);
    memcpy(cmd.params, params, params_size);

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void engine_abort(void)
{
    atomic_store(&s_abort_flag, true);
}

esp_err_t engine_zero(void)
{
    if (atomic_load(&s_scan_state) != SCAN_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    engine_cmd_t cmd = { .type = ECMD_ZERO };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

scan_state_t engine_get_state(void)
{
    return atomic_load(&s_scan_state);
}

esp_err_t engine_register_sink(const engine_sink_t *sink)
{
    if (!sink) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_sink_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_sink_count < ENGINE_MAX_SINKS) {
            s_sinks[s_sink_count++] = *sink;
            ret = ESP_OK;
        }
        xSemaphoreGive(s_sink_mutex);
    }
    return ret;
}

void engine_resync(const engine_sink_t *sink)
{
    if (!sink || !sink->on_resync) return;

    /* BUG FIX: the original code allocated snapshot[ENGINE_SCAN_BUF_MAX] on the
     * stack (1300 x 16 = 20.8 KB), which would overflow any task stack, especially
     * DispatcherTask (3 KB).  Use heap allocation sized to the actual point count
     * so the cost is proportional to the data, not the buffer capacity. */
    DataPoint    *snapshot = NULL;
    uint16_t      count    = 0;
    scan_state_t  state;

    if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        count = s_scan_buf_count;
        state = atomic_load(&s_scan_state);
        if (count > 0) {
            snapshot = malloc(count * sizeof(DataPoint));
            if (snapshot) {
                memcpy(snapshot, s_scan_buf, count * sizeof(DataPoint));
            } else {
                ESP_LOGW(TAG, "engine_resync: malloc failed for %u points", (unsigned)count);
                count = 0;
            }
        }
        xSemaphoreGive(s_buf_mutex);
    } else {
        ESP_LOGW(TAG, "engine_resync: could not acquire buf mutex");
        return;
    }

    sink->on_resync(snapshot, count, state, sink->ctx);
    free(snapshot); /* safe: free(NULL) is a no-op when count == 0 */
}

