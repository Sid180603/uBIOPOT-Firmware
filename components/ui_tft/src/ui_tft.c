/**
 * @file ui_tft.c
 * @brief On-device LVGL TFT UI — display init, encoder indev, engine sink.
 *
 * This file owns:
 *   1. SPI2_HOST bus + ILI9341 esp_lcd panel initialisation.
 *   2. LVGL port initialisation (task pinned to Core 0, 5 ms tick).
 *   3. Custom 2-button encoder indev:
 *      GPIO0  PRESS_DOWN  → enc_diff++ (navigate next)
 *      GPIO14 PRESS_DOWN  → enter pressed
 *      GPIO14 PRESS_UP    → enter released
 *      GPIO14 LONG_PRESS  → engine_abort() (abort running scan)
 *      GPIO0  10 s hold   → factory reset (handled by pstat_hal)
 *   4. Engine sink registration (on_point / on_event / on_resync).
 *   5. LED mirroring (READY LED ↔ engine IDLE; PROCESSING LED ↔ engine RUNNING).
 *
 * All LVGL calls from the sink are wrapped in lvgl_port_lock(0).
 * Screen transitions are handled by screen_mgr.c.
 */

#include "ui_tft.h"
#include "screen_mgr.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "pstat_hal/pstat_hal.h"
#include "acq_engine.h"
#include "echem_core/scan_state.h"
#include "echem_core/peaks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ui_tft";

/* =========================================================================
 * Pin config (from Kconfig.projbuild — all confirmed from physical board)
 * ========================================================================= */

#define TFT_SCLK   CONFIG_UBIOPOT_TFT_SCLK    /* GPIO15 */
#define TFT_MOSI   CONFIG_UBIOPOT_TFT_MOSI    /* GPIO2  */
#define TFT_CS     CONFIG_UBIOPOT_TFT_CS      /* GPIO5  */
#define TFT_DC     CONFIG_UBIOPOT_TFT_DC      /* GPIO4  */
#define TFT_PCLK   CONFIG_UBIOPOT_TFT_PCLK_HZ /* 40 MHz */

/* Landscape resolution: 320 wide × 240 high */
#define TFT_H_RES  320
#define TFT_V_RES  240

/*
 * DMA line-buffer height: 20 lines × 320 px × 2 bytes = 12,800 bytes.
 * Single-buffered (partial rendering, no animation overlap needed for a
 * low-fps settings/voltammogram UI). Saves ~38 KB of DMA-capable SRAM vs
 * the original 40-line double-buffer (51,200 bytes total), freeing it for
 * LVGL widget allocation on the same PSRAM-less WROOM-32.
 */
#define TFT_BUF_LINES 20
#define TFT_BUF_SIZE  (TFT_H_RES * TFT_BUF_LINES)

/* =========================================================================
 * Encoder indev state — written by iot_button callbacks, read by LVGL read_cb
 * ========================================================================= */

static lv_indev_t        *s_indev    = NULL;
static volatile int32_t   s_enc_diff = 0;    /* NAV press count since last read */
static volatile bool      s_btn_enter = false; /* START held down */

/* =========================================================================
 * TFT readiness flag + buffered WiFi info
 *
 * net_comms calls ui_tft_set_wifi_info() as soon as the SoftAP comes up,
 * which may be BEFORE lvgl_port_init() has run (depends on startup order).
 * We store the info here and apply it at the end of ui_tft_start().
 * ========================================================================= */
static atomic_bool s_tft_ready        = false;
static char        s_pending_ssid[64] = {0};
static char        s_pending_ip[32]   = {0};
static char        s_pending_url[64]  = {0};
static bool        s_wifi_info_pending = false;

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = s_enc_diff;
    data->state    = s_btn_enter ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_enc_diff     = 0;
}

/* ---- Button callbacks (called from iot_button task, NOT ISR context) ---- */

static void on_nav_press_down(void *handle, void *usr)
{
    (void)handle; (void)usr;
    s_enc_diff++;
    lvgl_port_task_wake(LVGL_PORT_EVENT_ENCODER, s_indev);
}

static void on_start_press_down(void *handle, void *usr)
{
    (void)handle; (void)usr;
    s_btn_enter = true;
    lvgl_port_task_wake(LVGL_PORT_EVENT_ENCODER, s_indev);
}

static void on_start_press_up(void *handle, void *usr)
{
    (void)handle; (void)usr;
    s_btn_enter = false;
    lvgl_port_task_wake(LVGL_PORT_EVENT_ENCODER, s_indev);
}

static void on_start_long_press(void *handle, void *usr)
{
    (void)handle; (void)usr;
    /* Abort the running scan.  engine_abort() is ISR/task safe (atomic flag). */
    engine_abort();
    ESP_LOGI(TAG, "START long-press: scan abort requested");
}

/* =========================================================================
 * Engine sink — receives DataPoints and events from DispatcherTask (Core 0)
 * ========================================================================= */

/* Maximum peaks to detect on-device (results screen shows up to 4) */
#define MAX_PEAKS 4

static peak_t      s_peaks[MAX_PEAKS];
static uint16_t    s_n_peaks  = 0;
static uint8_t     s_electrode = 1;

/* Scan buffer mirror for on-device peak detection after scan complete */
#define PEAK_BUF_MAX  ENGINE_SCAN_BUF_MAX
static float  s_E_buf[PEAK_BUF_MAX];
static float  s_I_buf[PEAK_BUF_MAX];
static uint16_t s_pt_count = 0;

static void sink_on_point(const DataPoint *pt, void *ctx)
{
    (void)ctx;
    /* scr_scan_push_point writes ONLY to the lock-free ring buffer (volatile
     * head/tail + plain array — single-producer here, single-consumer in the
     * LVGL flush timer).  No lvgl_port_lock() is taken: taking it here would
     * risk deadlock if the LVGL task is blocked waiting for Dispatcher output,
     * and is unnecessary because no LVGL object is touched directly.  The ring
     * buffer is the intentional decoupling mechanism for this hot path. */
    scr_scan_push_point(pt->E_mV, pt->I_uA);

    /* Mirror into local buffer for peak detection (written only by Dispatcher) */
    if (s_pt_count < PEAK_BUF_MAX) {
        s_E_buf[s_pt_count] = pt->E_mV;
        s_I_buf[s_pt_count] = pt->I_uA;
        s_pt_count++;
    }

    /* GPIO LED update — safe from any task, no LVGL involvement */
    pstat_led_set(PSTAT_LED_READY,      false);
    pstat_led_set(PSTAT_LED_PROCESSING, true);
}

static void sink_on_event(scan_event_t evt, const char *info, void *ctx)
{
    (void)ctx;

    /* Block indefinitely — events are rare and brief to process.
     * A 50 ms timeout would silently drop SCAN_EVT_SCAN_DONE / SCAN_EVT_ERROR,
     * leaving the UI stuck on the scan screen with LEDs in PROCESSING state. */
    if (!lvgl_port_lock(0)) return;

    switch (evt) {
        case SCAN_EVT_START:
            /* Sync electrode from home screen (BUG 1 fix: was always 1). */
            s_electrode = scr_home_get_electrode();
            s_pt_count = 0;
            float eb, ee;
            scr_home_get_e_range(&eb, &ee);
            scr_scan_reset(s_electrode, eb, ee);
            /* Show equilibration spinner immediately — DPV always equilibrates
             * first; SCAN_EVT_EQUILIB_DONE will clear it (ISSUE 2 fix). */
            scr_scan_set_equilibrating(true);
            scr_home_set_state(SCAN_STATE_RUNNING);
            pstat_led_set(PSTAT_LED_READY,      false);
            pstat_led_set(PSTAT_LED_PROCESSING, true);
            break;

        case SCAN_EVT_EQUILIB_DONE:
            scr_scan_set_equilibrating(false);
            break;

        case SCAN_EVT_SCAN_DONE: {
            scr_scan_stop_elapsed();
            /* Detect peaks from collected data */
            s_n_peaks = peaks_find(s_I_buf, s_E_buf, s_pt_count,
                                   s_peaks, MAX_PEAKS, 1.0f /* µA prominence */);
            /* Navigate first so the results screen is created (lazy) before
             * scr_results_set / scr_results_set_curve write into it.
             * Reversed order would silently drop data on the first scan. */
            screen_mgr_goto_results();
            scr_results_set(s_peaks, s_n_peaks, s_electrode);
            /* Pass raw curve for mini voltammogram (#12) */
            scr_results_set_curve(s_E_buf, s_I_buf, s_pt_count);
            scr_home_set_state(SCAN_STATE_IDLE);
            pstat_led_set(PSTAT_LED_READY,      true);
            pstat_led_set(PSTAT_LED_PROCESSING, false);
            break;
        }

        case SCAN_EVT_ABORTED:
            scr_scan_stop_elapsed();
            scr_home_set_state(SCAN_STATE_IDLE);
            screen_mgr_goto_home();
            screen_mgr_show_toast("Scan aborted", TOAST_INFO);
            pstat_led_set(PSTAT_LED_READY,      true);
            pstat_led_set(PSTAT_LED_PROCESSING, false);
            break;

        case SCAN_EVT_ERROR:
            scr_home_set_state(SCAN_STATE_ERROR);
            screen_mgr_goto_home();
            if (info) {
                screen_mgr_show_toast(info, TOAST_ERROR);
            } else {
                screen_mgr_show_toast("Scan error \u2014 check params", TOAST_ERROR);
            }
            pstat_led_set(PSTAT_LED_READY,      true);
            pstat_led_set(PSTAT_LED_PROCESSING, false);
            break;

        case SCAN_EVT_RESET:
            scr_home_set_state(SCAN_STATE_IDLE);
            pstat_led_set(PSTAT_LED_READY,      true);
            pstat_led_set(PSTAT_LED_PROCESSING, false);
            break;
    }

    lvgl_port_unlock();
}

static void sink_on_resync(const DataPoint *buf, uint16_t count,
                           scan_state_t state, void *ctx)
{
    (void)ctx;

    /* Block indefinitely — resync is called once at startup; a failed acquire
     * would leave the chart blank with no retry, silently losing the replay. */
    if (!lvgl_port_lock(0)) return;

    if (count == 0 || buf == NULL) {
        lvgl_port_unlock();
        return;
    }

    /* Reconstruct scan: reset chart and replay all buffered points */
    float eb, ee;
    scr_home_get_e_range(&eb, &ee);
    scr_scan_reset(s_electrode, eb, ee);
    s_pt_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        scr_scan_push_point(buf[i].E_mV, buf[i].I_uA);
        if (s_pt_count < PEAK_BUF_MAX) {
            s_E_buf[s_pt_count] = buf[i].E_mV;
            s_I_buf[s_pt_count] = buf[i].I_uA;
            s_pt_count++;
        }
    }

    if (state == SCAN_STATE_RUNNING || state == SCAN_STATE_EQUILIBRATING) {
        screen_mgr_goto_scan();
    }

    lvgl_port_unlock();
}

static const engine_sink_t s_tft_sink = {
    .on_point  = sink_on_point,
    .on_event  = sink_on_event,
    .on_resync = sink_on_resync,
    .ctx       = NULL,
};

/* =========================================================================
 * ui_tft_start
 * ========================================================================= */

esp_err_t ui_tft_start(void)
{
    esp_err_t ret;

    /* ------------------------------------------------------------------
     * 1. SPI2_HOST bus (HSPI) — separate from SPI3_HOST used by MCP4921
     * ------------------------------------------------------------------ */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = TFT_SCLK,
        .mosi_io_num     = TFT_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = TFT_BUF_SIZE * sizeof(uint16_t),
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(SPI2) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ------------------------------------------------------------------
     * 2. Panel IO (SPI)
     * ------------------------------------------------------------------ */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = TFT_CS,
        .dc_gpio_num       = TFT_DC,
        .spi_mode          = 0,
        .pclk_hz           = TFT_PCLK,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx          = NULL,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ------------------------------------------------------------------
     * 3. ILI9341 panel
     *    RST = -1  → software reset via SPI command 0x01 on panel_reset().
     *    MISO = -1 → write-only panel (no readback, no touch).
     *    BGR colour order as ILI9341 native.
     * ------------------------------------------------------------------ */
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_reset(panel_handle);   /* issues SPI SW-reset cmd 0x01 */
    esp_lcd_panel_init(panel_handle);

    /* esp_lvgl_port owns MADCTL (rotation section in disp_cfg below). */
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    ESP_LOGI(TAG, "ILI9341 panel init OK (SCLK=%d MOSI=%d CS=%d DC=%d)",
             TFT_SCLK, TFT_MOSI, TFT_CS, TFT_DC);

    /* ------------------------------------------------------------------
     * 4. LVGL port init (task pinned to Core 0, 5 ms tick)
     * ------------------------------------------------------------------ */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity    = 0;          /* Core 0 — same as WiFi/comms */
    port_cfg.task_priority    = 4;          /* Same as default */
    port_cfg.task_stack       = 8192;       /* 8 KB: LVGL + screen objects */
    port_cfg.timer_period_ms  = 5;

    ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ------------------------------------------------------------------
     * 5. Display (partial DMA, swap_bytes for ILI9341 big-endian, landscape)
     * ------------------------------------------------------------------ */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = panel_handle,
        .buffer_size   = TFT_BUF_SIZE,
        .double_buffer = false,
        .hres          = TFT_H_RES,
        .vres          = TFT_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = true,    /* landscape: port applies MV → panel 320 wide */
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma    = true,    /* DMA-capable SRAM buffers */
            .buff_spiram = false,
            .sw_rotate   = false,   /* HW rotate via MADCTL (done above) */
            .swap_bytes  = true,    /* ILI9341 is big-endian RGB565 */
            .full_refresh = false,
            .direct_mode  = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    /* ------------------------------------------------------------------
     * 6. Encoder indev (2-button: NAV=rotate, START=enter).
     *    screen_mgr_init creates per-screen groups and assigns them to the
     *    indev on each transition — no single shared group needed here.
     * ------------------------------------------------------------------ */
    if (!lvgl_port_lock(0)) return ESP_FAIL;

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(s_indev, encoder_read_cb);
    lv_indev_set_disp(s_indev, disp);
    /* Group assigned by screen_mgr_init below (one per screen). */

    /* ------------------------------------------------------------------
     * 7. All screens — pass indev so screen_mgr can switch groups
     * ------------------------------------------------------------------ */
    screen_mgr_init(disp, s_indev);

    lvgl_port_unlock();

    /* ------------------------------------------------------------------
     * 8. Button callbacks — must be AFTER lvgl_port_init so task_wake works
     * ------------------------------------------------------------------ */
    pstat_button_nav_on_press_down(on_nav_press_down, NULL);
    pstat_button_start_on_press_down(on_start_press_down, NULL);
    pstat_button_start_on_press_up(on_start_press_up, NULL);
    pstat_button_start_on_long_press(on_start_long_press, NULL);

    /* ------------------------------------------------------------------
     * 9. Register as engine sink, then resync (catches in-progress scan)
     * ------------------------------------------------------------------ */
    ret = engine_register_sink(&s_tft_sink);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "engine_register_sink failed: %s", esp_err_to_name(ret));
        /* Non-fatal: UI will work but won't receive scan data */
    }
    engine_resync((engine_sink_t *)&s_tft_sink);

    /* ------------------------------------------------------------------
     * 10. Initial LED state
     * ------------------------------------------------------------------ */
    pstat_led_set(PSTAT_LED_READY,      true);
    pstat_led_set(PSTAT_LED_PROCESSING, false);

    /* Mark LVGL as ready, then flush any WiFi info that arrived early */
    atomic_store(&s_tft_ready, true);
    if (s_wifi_info_pending) {
        if (lvgl_port_lock(0)) {
            scr_settings_set_wifi(s_pending_ssid, s_pending_ip, s_pending_url);
            lvgl_port_unlock();
        }
        s_wifi_info_pending = false;
    }

    ESP_LOGI(TAG, "ui_tft_start complete — Splash screen active");
    return ESP_OK;
}

/* =========================================================================
 * ui_tft_set_wifi_info — called by net_comms (P5) once WiFi is up
 * ========================================================================= */

void ui_tft_set_wifi_info(const char *ssid, const char *ip, const char *url)
{
    if (!atomic_load(&s_tft_ready)) {
        /* LVGL not initialised yet — buffer the info for ui_tft_start() to apply */
        if (ssid) strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
        if (ip)   strlcpy(s_pending_ip,   ip,   sizeof(s_pending_ip));
        if (url)  strlcpy(s_pending_url,  url,  sizeof(s_pending_url));
        s_wifi_info_pending = true;
        return;
    }
    if (!lvgl_port_lock(0)) return;
    scr_settings_set_wifi(ssid, ip, url);
    lvgl_port_unlock();
}

esp_err_t ui_tft_request_nav(ui_screen_t screen)
{
    if (!atomic_load(&s_tft_ready)) return ESP_ERR_INVALID_STATE;
    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;
    switch (screen) {
        case UI_SCREEN_HOME:     screen_mgr_goto_home();     break;
        case UI_SCREEN_SCAN:     screen_mgr_goto_scan();     break;
        case UI_SCREEN_RESULTS:  screen_mgr_goto_results();  break;
        case UI_SCREEN_SETTINGS: screen_mgr_goto_settings(); break;
        default:
            lvgl_port_unlock();
            return ESP_ERR_INVALID_ARG;
    }
    lvgl_port_unlock();
    return ESP_OK;
}

