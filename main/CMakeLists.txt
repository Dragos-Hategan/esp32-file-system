idf_component_register(SRCS "main.c"
                    REQUIRES 
                        touch_xpt2046
                        file_browser
                        esp_bsp_generic
                    )
