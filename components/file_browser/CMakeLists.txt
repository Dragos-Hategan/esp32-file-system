idf_component_register(
    SRCS "text_viewer_screen.c" "file_browser.c" "fs_navigator.c" "fs_text_ops.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_bsp_generic 
        esp_common     
        lvgl
        sd_card
    PRIV_REQUIRES
        esp_driver_sdspi
        esp_hw_support  
        nvs_flash       
        fatfs           
        sdmmc           
)
