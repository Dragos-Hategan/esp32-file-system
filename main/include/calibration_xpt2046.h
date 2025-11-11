#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_touch.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#ifndef UI_H
#define UI_H

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
 * @brief Get a pointer to the current touch calibration structure.
 *
 * @return const touch_cal_t* Pointer to internal calibration data (read-only).
 */
const touch_cal_t *touch_get_cal(void);

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
 * @see touch_get_cal()
 * @see touch_cal_load_nvs()
 */
void load_nvs_calibration(bool *calibration_found);

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

/**
 * @brief Display a full screen with a centered text message.
 *
 * Places a centered label, updates layout, and forces an immediate refresh.
 *
 * @param txt Null-terminated string to show in the center.
 *
 * @note Runs on the LVGL thread/context; make sure your platform’s display
 *       locking rules are respected before calling if required.
 */
void ui_show_calibration_message(void);

/**
 * @brief Show a modal Yes/No dialog with a 5-second auto-Yes countdown.
 *
 * Creates an LVGL message box centered on the active screen with the provided
 * question text and two buttons: **Yes** and **No**. Under the dialog it
 * displays a compact container with the text "Performing Calibration" plus a
 * circular progress arc and a numeric countdown (5 → 1).  
 * The function blocks the calling task until the user presses a button or the
 * 5-second timeout elapses. On timeout, the result is treated as **Yes**.
 *
 * Thread-safety: internal calls to @ref bsp_display_lock / @ref bsp_display_unlock
 * protect LVGL operations. This function must be called from a task context
 * (not from an ISR) after LVGL has been initialized and a display is active.
 *
 * External dependencies:
 *  - FreeRTOS (semaphores, delays, ticks)
 *  - LVGL (objects, message box, arc, labels, refresh)
 *  - A user-provided event callback `event_cb` that writes the button result to
 *    a @c msg_box_response_t passed via @c user_data and gives the binary semaphore.
 *
 * @param question Null-terminated string displayed inside the message box.
 * @return bool
 * @retval true  if the user pressed **Yes** or the countdown timed out
 * @retval false if the user pressed **No** 
 *
 * @note The countdown duration is fixed at 5000 ms in this implementation.
 * @warning The function performs blocking waits (semaphore / vTaskDelay).
 * @warning This function assumes there is no LVGL display lock already acquired.
 */
bool ui_yes_no_dialog(const char *question);

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

#endif // UI_H