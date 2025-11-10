// touch_xpt2046.h
#ifndef TOUCH_XPT2046_H
#define TOUCH_XPT2046_H

#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "driver/spi_master.h"

/* ---------------------- XPT2046 TOUCH CONFIG ---------------------- */
// SPI Config
#define TOUCH_SPI_HOST      SPI3_HOST
#define TOUCH_SPI_SCLK_IO   GPIO_NUM_4
#define TOUCH_CS_IO         GPIO_NUM_5
#define TOUCH_SPI_MOSI_IO   GPIO_NUM_6
#define TOUCH_SPI_MISO_IO   GPIO_NUM_7

// IRQ + RST:
#define TOUCH_IRQ_IO        GPIO_NUM_1      // active LOW on XPT2046
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

typedef struct {
    float xA, xB, xC;   // for x' = xA*x + xB*y + xC
    float yA, yB, yC;   // for y' = yA*x + yB*y + yC
    bool valid;
    uint32_t magic;     // 0xC411B007
    uint32_t crc32;     // simple, for integrity
} touch_cal_t;

typedef struct{ 
    int tx;
    int ty; 
    int rx;
    int ry; 
} cal_point_t;

/**
 * @brief Get the global touch handle created by the XPT2046 driver.
 *
 * @return esp_lcd_touch_handle_t Current touch handle or NULL if not initialized.
 */
esp_lcd_touch_handle_t touch_get_handle(void);

/**
 * @brief Get a pointer to the current touch calibration structure.
 *
 * @return const touch_cal_t* Pointer to internal calibration data (read-only).
 */
const touch_cal_t *touch_get_cal(void);

/**
 * @brief Initialize SPI and create the XPT2046 touch driver (esp_lcd_touch).
 *
 * Sets up the SPI bus for the touch controller, creates the panel IO handle,
 * and instantiates the XPT2046 touch driver with orientation flags.
 *
 * @note On success, a global handle is stored and can be retrieved with touch_get_handle().
 */
void init_touch(void);

/**
 * @brief Register the touch controller as an LVGL pointer device.
 *
 * Creates an LVGL input device, sets it to pointer type, and attaches the
 * @ref lvgl_touch_read_cb callback.
 *
 * @return lv_indev_t* LVGL input device handle.
 */
lv_indev_t *register_touch_with_lvgl(void);

/**
 * @brief Save a valid touch calibration to NVS with CRC protection.
 *
 * Writes a blob identified by @c TOUCH_CAL_NVS_NS / @c TOUCH_CAL_NVS_KEY.
 * A magic and CRC-32 are included for integrity checks.
 *
 * @param cal Pointer to a valid calibration structure (cal->valid must be true).
 * @return esp_err_t ESP_OK on success or an error code from NVS APIs.
 */
esp_err_t touch_cal_save_nvs(const touch_cal_t *cal);

/**
 * @brief Load touch calibration from NVS into the internal state.
 *
 * Reads the calibration blob, validates magic and CRC, and updates @ref s_cal.
 *
 * @param existing_cal Non-NULL pointer (unused for output here; required to keep signature uniform).
 * @return true if a valid calibration was loaded, false otherwise.
 */
bool touch_cal_load_nvs(const touch_cal_t *existing_cal);

/**
 * @brief Run a 5-point on-screen calibration flow and persist the result to NVS.
 *
 * Shows a temporary calibration screen, renders crosshairs at 5 targets,
 * samples raw coordinates, then solves for an affine transform that maps
 * raw (x,y) to screen (x',y'):
 * @code
 * x' = xA*x + xB*y + xC
 * y' = yA*x + yB*y + yC
 * @endcode
 *
 * The coefficients are solved via least squares over the 5 samples (normal equations),
 * checking for a near-singular system. On success, @ref s_cal is marked valid and saved.
 * 
 * @warning This function assumes there is no LVGL display lock already acquired.
 */
void run_5point_touch_calibration(void);

/**
 * @brief Read raw (x,y) from the touch controller by averaging multiple samples.
 *
 * Performs 12 reads spaced by ~15 ms, averages them, and returns integer raw coordinates.
 *
 * @param[out] rx Averaged raw X.
 * @param[out] ry Averaged raw Y.
 * 
 */
void sample_raw(int *rx, int *ry);

#endif // TOUCH_XPT2046_H