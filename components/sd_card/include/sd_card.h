#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

typedef enum{
    SDSPI_SUCCESS,
    SDSPI_SPI_BUS_FAILED,
    SDSPI_FAT_MOUNT_FAILED,
    SDSPI_COMMUNICATION_FAILED
} sdspi_failcodes_t;

typedef struct{
    sdspi_failcodes_t sdspi_failcode;
    esp_err_t esp_err;
} sdspi_result_t ;

/**
 * @brief Initializes the SDSPI bus and mounts the SD card filesystem.
 *
 * Performs one-time initialization of the SPI bus configured by
 * @c CONFIG_SDSPI_BUS_HOST, then attempts to mount the FAT filesystem at
 * @c CONFIG_SDSPI_MOUNT_POINT via @c esp_vfs_fat_sdspi_mount.
 *
 * The function is safe to call multiple times. If the SPI bus was already
 * initialized or the card is already mounted, the call returns a success
 * result without modifying the existing state.
 *
 * @return sdspi_result_t
 *         A structured result containing:
 *         - a high-level SDSPI fail code (see @ref sdspi_failcode_t), and
 *         - the underlying @c esp_err_t value returned by the ESP-IDF API.
 *
 * @note No fatal aborts are used; all errors are reported through the returned
 *       @c sdspi_result_t structure.
 *
 * @post On success, the VFS mount point is ready.
 */
sdspi_result_t init_sdspi(void);

void sdspi_fallback(sdspi_result_t res);

#ifdef __cplusplus
}
#endif
