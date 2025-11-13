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
 * @brief Create and show the file browser screen.
 *
 * Initializes the navigation context with @p cfg->root_path and builds an LVGL screen
 * containing the path label, sort controls, and an entry list. The screen is loaded
 * immediately.
 *
 * @param[in] cfg File browser configuration (must contain a valid @c root_path).
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if @p cfg or @p cfg->root_path is invalid
 * - Error from fs_nav_init on navigation setup failure
 * - ESP_ERR_TIMEOUT if display lock cannot be acquired
 *
 * @post On success, internal context is initialized and visible on the display.
 */
esp_err_t file_browser_start(const file_browser_config_t *cfg);

#ifdef __cplusplus
}
#endif
