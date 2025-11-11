idf_component_register(SRCS "file_browser.c" "fs_navigator.c" "calibration_xpt2046.c" "esp32-file-display.c" "touch_xpt2046.c"
                    INCLUDE_DIRS "include"
                    REQUIRES 
                        esp_bsp_generic
                        esp_driver_spi 
                        esp_driver_sdspi
                        nvs_flash
                        sdmmc
                        fatfs
                    )
