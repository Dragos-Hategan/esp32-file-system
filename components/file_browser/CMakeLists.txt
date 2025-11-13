idf_component_register(
    SRCS "file_browser.c" "fs_navigator.c" "fs_text_ops.c" "text_editor_screen.c" "text_viewer_screen.c"
    INCLUDE_DIRS "include"
    REQUIRES
    nvs_flash
    esp_bsp_generic
)