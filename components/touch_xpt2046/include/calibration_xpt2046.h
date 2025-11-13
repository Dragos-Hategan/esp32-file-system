#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_touch.h"
#include "lvgl.h"

/**
 * @brief Load touch calibration data from NVS and indicate whether it was found.
 *
 * This function attempts to read previously saved touch calibration data from NVS.
 * It checks if valid calibration data exists and updates the provided flag accordingly.
 * If no calibration is found, the system will print a message indicating that a new
 * calibration is required.
 *
 * @param[out] calibration_found Pointer to a boolean that will be set to `true`
 * if valid calibration data was found, or `false` otherwise.
 *
 * @note This function relies on the touch calibration subsystem and should be called
 * after `init_nvs()` and touch driver initialization.
 *
 * @see touch_cal_load_nvs()
 */
void load_nvs_calibration(bool *calibration_found);

/**
 * @brief Run or skip the touch screen calibration process based on stored data.
 *
 * This function checks whether valid touch calibration data already exists.
 * If no calibration is found, a 5-point touch calibration is started immediately.
 * If calibration data is available, the user is prompted (via a Yes/No LVGL dialog)
 * to decide whether to run a new calibration.
 *
 * When the user chooses not to recalibrate, the existing calibration data
 * (previously loaded from NVS) is kept, and the display is cleared for a clean UI state.
 *
 * @param[in] calibration_found
 *        Indicates whether valid calibration data was loaded from NVS (`true`)
 *        or not (`false`).
 *
 * @note This function assumes that the LVGL UI and display driver are already initialized.
 * @note The function uses @ref bsp_display_lock and @ref bsp_display_unlock to ensure
 *       thread-safe access to the display.
 */
void calibration_test(bool calibration_found);

/**
 * @brief Apply current touch calibration to a raw (x,y) reading.
 *
 * If no valid calibration is available, the raw coordinates are clamped to the display bounds.
 * Otherwise, an affine transform is applied:
 * @code
 * x' = xA*x + xB*y + xC
 * y' = yA*x + yB*y + yC
 * @endcode
 *
 * @param raw_x Raw X from controller.
 * @param raw_y Raw Y from controller.
 * @param[out] out_point Output LVGL point (screen space).
 * @param xmax Screen width (max X, exclusive).
 * @param ymax Screen height (max Y, exclusive).
 */
void apply_touch_calibration(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point, int xmax, int ymax);

#ifdef __cplusplus
}
#endif