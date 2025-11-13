idf_component_register(
    SRCS "touch_xpt2046.c" "calibration_xpt2046.c"
    INCLUDE_DIRS "include"
    REQUIRES
    esp_lcd_touch_xpt2046
    esp_bsp_generic
    esp_driver_spi
    nvs_flash
)