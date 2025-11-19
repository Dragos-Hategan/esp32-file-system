idf_component_register(
    SRCS "touch_xpt2046.c" "calibration_xpt2046.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_lcd_touch_xpt2046
        esp_lcd_touch
        esp_common
        lvgl
    PRIV_REQUIRES
        esp_bsp_generic
        esp_driver_spi
        file_browser
        nvs_flash
        freertos
)