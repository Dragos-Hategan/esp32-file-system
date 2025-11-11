// touch_xpt2046.h
#ifndef TOUCH_XPT2046_H
#define TOUCH_XPT2046_H

#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "driver/spi_master.h"
#include "file_browser.h"

/* ---------------------- XPT2046 TOUCH CONFIG ---------------------- */
// SPI Config
#define TOUCH_SPI_HOST      BSP_LCD_SPI_NUM
#define TOUCH_SPI_MISO_IO   CONFIG_BSP_DISPLAY_MISO_GPIO
#define TOUCH_SPI_MOSI_IO   CONFIG_BSP_DISPLAY_MOSI_GPIO
#define TOUCH_SPI_SCLK_IO   CONFIG_BSP_DISPLAY_SCLK_GPIO
#define TOUCH_CS_IO         GPIO_NUM_8
#define TOUCH_IRQ_IO        GPIO_NUM_11      // active LOW on XPT2046

#define TOUCH_RST_IO        -1     

// Panel dimensions and orientation
#define TOUCH_X_MAX         320
#define TOUCH_Y_MAX         240
#define TOUCH_SWAP_XY       true   // for landscape with ILI9341
#define TOUCH_MIRROR_X      true
#define TOUCH_MIRROR_Y      true

#define TOUCH_SPI_HZ        (2 * 1000 * 1000)

#define TOUCH_CAL_NVS_NS     "touch_cal"
#define TOUCH_CAL_NVS_KEY    "affine_v1"
/* ---------------------- XPT2046 TOUCH CONFIG ---------------------- */

/**
 * @brief Get a pointer to the global touch input device created by lv_indev_create().
 *
 * @return lv_indev_t Current touch input device pointer or NULL if not initialized.
 */
lv_indev_t *touch_get_indev(void);

/**
 * @brief Get the global touch handle created by the XPT2046 driver.
 *
 * @return esp_lcd_touch_handle_t Current touch handle or NULL if not initialized.
 */
esp_lcd_touch_handle_t touch_get_handle(void);

/**
 * @brief Initialize the SPI bus and create the XPT2046 touch driver.
 *
 * This function sets up the SPI bus used by the XPT2046 touch controller,
 * creates the `esp_lcd_panel_io` handle, and initializes the XPT2046 touch driver
 * through the `esp_lcd_touch` API.  
 * It also applies orientation flags such as axis swap and mirroring according
 * to the configuration macros (`TOUCH_SWAP_XY`, `TOUCH_MIRROR_X`, `TOUCH_MIRROR_Y`).
 *
 * If the function is called multiple times, initialization will only be performed once.
 * On success, a global handle to the touch driver is stored internally and can be
 * retrieved later using `touch_get_handle()`.
 *
 * @return
 * - ESP_OK on successful initialization  
 * - Error code from underlying ESP-IDF driver functions if initialization fails  
 *   (for example: @c ESP_ERR_NO_MEM, @c ESP_ERR_INVALID_ARG, etc.)
 *
 * @note The SPI bus is initialized with automatic DMA channel selection.
 * @note The XPT2046 interrupt pin is expected to be active-low.
 * @note The recommended SPI clock for XPT2046 is typically ≤ 2.5 MHz.
 *
 */
esp_err_t init_touch(void);

/**
 * @brief Register the XPT2046 touch controller with LVGL.
 *
 * Locks the BSP display (LVGL draw context), creates and registers the touch
 * input device via @ref register_touch_with_lvgl, and then attaches the read
 * callback so LVGL can fetch touch events.
 *
 * The display lock is released on all code paths.
 *
 * @return
 * - true  : touch input device was created/registered successfully
 * - false : registration failed (e.g., driver not initialized or allocation error)
 *
 * @note Must be called after LVGL and the display/panel drivers are initialized.
 * @note `bsp_display_lock(0)` waits indefinitely for the lock (thread-safe).
 */
bool register_touch_to_lvgl(void);

#endif // TOUCH_XPT2046_H