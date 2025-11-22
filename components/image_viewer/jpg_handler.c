#include "jpg_handler.h"

#include <string.h>

#include "esp_log.h"

#define TAG "jpg_handler"

esp_err_t jpg_handler_set_src(lv_obj_t *img, const char *path)
{
    if (!img || !path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lv_image_set_src(img, path);

    return ESP_OK;
}
