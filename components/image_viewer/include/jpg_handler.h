#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Set the LVGL image source to a JPEG on disk.
 *
 * Performs basic validation (non-empty path) before calling lv_image_set_src().
 *
 * @param img  LVGL image object (must be non-NULL).
 * @param path Path to JPEG file (drive-prefixed if using LVGL FS, e.g. "S:/...").
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad inputs.
 */
esp_err_t jpg_handler_set_src(lv_obj_t *img, const char *path);

#ifdef __cplusplus
}
#endif
