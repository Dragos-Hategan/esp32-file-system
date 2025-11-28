#include "settings.h"

#include <stddef.h>

#include "lvgl.h"
#include "esp_log.h"

typedef struct{
    int incr_90_deg;
}settings_t;

static settings_t s_settings;
static const char *TAG = "settings";

void init_settings(void)
{
    s_settings.incr_90_deg = 0;
}

void settings_rotate_screen(void)
{
    lv_display_t *display = lv_display_get_default();
    if (!display){
        return;
    }

    switch(s_settings.incr_90_deg){
        case 0:
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_270);
            s_settings.incr_90_deg = 1;
            break;
        case 1:
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180);
            s_settings.incr_90_deg = 2;        
            break;
        case 2:
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
            s_settings.incr_90_deg = 3;        
            break;
        case 3:
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
            s_settings.incr_90_deg = 0;        
            break; 
        
        default:
            ESP_LOGE(TAG, "Wrong rotation setting");
            break;
    }
}