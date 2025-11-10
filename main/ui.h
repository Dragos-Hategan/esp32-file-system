#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_touch.h"

#ifndef UI_H
#define UI_H

/**
 * @brief Display a full-white screen with a centered text message.
 *
 * Clears the active screen, sets a white background, places a centered label,
 * updates layout, and forces an immediate refresh.
 *
 * @param txt Null-terminated string to show in the center.
 *
 * @note Runs on the LVGL thread/context; make sure your platform’s display
 *       locking rules are respected before calling if required.
 */
void ui_show_message(const char *txt);

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
 * @brief Draw a four-arrow crosshair pointing to a target coordinate.
 *
 * Clears the active screen and draws four black arrows (up/down/left/right)
 * converging toward (@p x, @p y). A shared line style is lazily initialized
 * on first call. Forces an immediate display refresh when done.
 *
 * @param x Target X coordinate in screen space.
 * @param y Target Y coordinate in screen space.
 *
 * @note Designed for LVGL v9. Adjust types/APIs if using a different LVGL major version.
 */
void draw_cross(int x, int y);

#endif // UI_H