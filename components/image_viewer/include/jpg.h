#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    const char *path;          /**< Absolute or drive-prefixed path to JPEG file (e.g. "S:/img.jpg"). */
    lv_obj_t *return_screen;   /**< Screen to return to when closing the viewer (may be NULL). */
} jpg_viewer_open_opts_t;

/**
 * @brief Open a simple viewer screen that displays a JPEG file.
 *
 * The viewer builds a new LVGL screen with a close button and an image widget
 * whose source is the provided @p path. On close, it returns to @p return_screen
 * if provided; otherwise it loads the previously active screen.
 *
 * @param opts Options struct (must not be NULL); @p path must be non-empty.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad input,
 *         ESP_ERR_NOT_FOUND if the file is missing, ESP_ERR_TIMEOUT if display
 *         lock cannot be acquired, or ESP_FAIL on LVGL source set failure.
 */
esp_err_t jpg_viewer_open(const jpg_viewer_open_opts_t *opts);

#ifdef __cplusplus
}
#endif
