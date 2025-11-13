idf_component_register(
    SRCS "file_browser.c" "fs_navigator.c" "fs_text_ops.c" "text_editor_screen.c" "text_viewer_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_bsp_generic 
        esp_common     
        lvgl          
    PRIV_REQUIRES
        esp_driver_sdspi
        esp_hw_support  
        nvs_flash       
        fatfs           
        sdmmc           
)