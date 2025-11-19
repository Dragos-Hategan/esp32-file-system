#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"
#include "sd_card.h"

typedef struct {
    const char *root_path;
    size_t max_entries;
} file_browser_config_t;

/**
 * @brief Create the LVGL file-browser screen using the default SDSPI root.
 *
 * Initializes the internal navigation context with `CONFIG_SDSPI_MOUNT_POINT`,
 * builds the LVGL widgets, and loads the screen. The SD card must already
 * be mounted via `init_sdspi()`.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if `CONFIG_SDSPI_MOUNT_POINT` is NULL
 * - Errors propagated from `fs_nav_init`
 * - ESP_ERR_TIMEOUT if the LVGL display lock cannot be acquired
 */
esp_err_t file_browser_start(void);

#ifdef __cplusplus
}
#endif
