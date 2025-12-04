idf_component_register(
    SRCS "file_manager.c" "text_viewer_screen.c" "fs_navigator.c" "fs_text_ops.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_bsp_generic 
        esp_common     
        lvgl
        image_viewer
        sd_card
        fonts
    PRIV_REQUIRES
        esp_driver_sdspi
        esp_hw_support
        nvs_flash       
        esp_timer  
        settings
        fatfs           
        sdmmc           
)
