idf_component_register(SRCS "main.c"
                    REQUIRES 
                        esp_bsp_generic
                        touch_xpt2046
                        file_browser
                        nvs_flash
                    )
