#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include "esp_err.h"

extern SemaphoreHandle_t reconnection_success;

/**
 * @brief Initialize (or reinitialize) the SDSPI bus and mount the SD card filesystem.
 *
 * Performs SPI bus setup and mounts FAT at @c CONFIG_SDSPI_MOUNT_POINT via
 * `esp_vfs_fat_sdspi_mount`. If a card is already mounted or the bus is active,
 * they are gracefully unmounted/freed before attempting again.
 *
 * @return esp_err_t
 *         - ESP_OK on success
 *         - ESP-IDF error code if SPI bus initialization or FAT mount fails
 */
esp_err_t init_sdspi(void);

/**
 * @brief Prompt the user then retry SD card initialization with UI feedback.
 */
void retry_init_sdspi(void);

 /**
 * @brief Launch the SD retry worker task if one is not already running.
 */
void sdspi_schedule_sd_retry(void);

#ifdef __cplusplus
}
#endif
