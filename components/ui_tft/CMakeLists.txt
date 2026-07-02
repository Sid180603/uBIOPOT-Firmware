## ui_tft skeleton — filled in P4.
## LVGL on-device UI: ILI9341 240x320 SPI TFT, 2-button encoder navigation,
## splash/home/scan-live/results/settings/toast screens, live voltammogram.
## esp_lcd_ili9341 + esp_lvgl_port pulled in via main/idf_component.yml managed components.

idf_component_register(
    SRCS "src/ui_tft.c"
    INCLUDE_DIRS "include"
    REQUIRES echem_core acq_engine
)
