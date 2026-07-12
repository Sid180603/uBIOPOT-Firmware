## ui_tft — On-device LVGL TFT UI (P4).
## ILI9341 240x320 SPI TFT, landscape 320x240, 2-button encoder navigation.
## Screens: Splash / Home-Menu / Scan-Live (live voltammogram) / Results / Settings / Toast.
## esp_lcd_ili9341 + esp_lvgl_port pulled in via main/idf_component.yml managed components.

idf_component_register(
    SRCS
        "src/ui_tft.c"
        "src/screen_mgr.c"
        "src/scr_splash.c"
        "src/scr_home.c"
        "src/scr_scan.c"
        "src/scr_results.c"
        "src/scr_settings.c"
        "src/scr_toast.c"
    INCLUDE_DIRS     "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        echem_core
        acq_engine
        pstat_hal
        espressif__esp_lvgl_port
        espressif__esp_lcd_ili9341
        esp_lcd
        esp_driver_spi
        esp_system
        log
)
