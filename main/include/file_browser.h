// main/file_browser.h
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "touch_xpt2046.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------- SD SPI CONFIG ---------------------- */

#define SDSPI_MOUNT_POINT         "/sdcard"
#define SDSPI_BUS_HOST            SPI3_HOST
#define SPSPI_BUS_SCL_PIN         GPIO_NUM_1
#define SDSPI_BUS_MOSI_PIN        GPIO_NUM_2
#define SDSPI_DEVICE_CS_PIN       GPIO_NUM_41
#define SDSPI_BUS_MISO_PIN        GPIO_NUM_42
#define SDMMC_HOST_MAX_FREQ_KHZ (5 * 1000)

/* ---------------------- SD SPI CONFIG ---------------------- */

typedef struct {
    const char *root_path;
    size_t max_entries;
} file_browser_config_t;

void init_sdspi(void);
esp_err_t file_browser_start(const file_browser_config_t *cfg);
esp_err_t file_browser_reload(void);

#ifdef __cplusplus
}
#endif
