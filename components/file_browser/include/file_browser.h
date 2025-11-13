#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"

/* ---------------------- SD SPI CONFIG ---------------------- */

#define SDSPI_MOUNT_POINT         "/sdcard"
#define SDSPI_BUS_HOST            SPI3_HOST
#define SPSPI_BUS_SCL_PIN         GPIO_NUM_1
#define SDSPI_BUS_MOSI_PIN        GPIO_NUM_2
#define SDSPI_DEVICE_CS_PIN       GPIO_NUM_41
#define SDSPI_BUS_MISO_PIN        GPIO_NUM_42
#define SDSPI_MAX_FREQ_KHZ        SDMMC_FREQ_DEFAULT // stable only with proper pullups

/* ---------------------- SD SPI CONFIG ---------------------- */

typedef struct {
    const char *root_path;
    size_t max_entries;
} file_browser_config_t;

/**
 * @brief Initialize and mount SD card over SDSPI (if not already mounted).
 *
 * Initializes the SPI bus (id = @c SDSPI_BUS_HOST) once, then mounts the FAT filesystem at
 * @c SDSPI_MOUNT_POINT using @c esp_vfs_fat_sdspi_mount. Safe to call multiple times.
 *
 * @note Calls ESP_ERROR_CHECK on unrecoverable errors.
 * @post On success, @c s_sd_card is non-NULL and VFS is mounted.
 */
void init_sdspi(void);

/**
 * @brief Create the LVGL file-browser screen using the default SDSPI root.
 *
 * Initializes the internal navigation context with `SDSPI_MOUNT_POINT`,
 * builds the LVGL widgets, and loads the screen. The SD card must already
 * be mounted via `init_sdspi()`.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if `SDSPI_MOUNT_POINT` is NULL
 * - Errors propagated from `fs_nav_init`
 * - ESP_ERR_TIMEOUT if the LVGL display lock cannot be acquired
 */
esp_err_t file_browser_start(void);

#ifdef __cplusplus
}
#endif
