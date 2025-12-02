#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Perform one-time system bring-up (NVS, display, touch, SD) and UI calibration.
 *
 * Initializes NVS, display/LVGL (backlight + default Domine font theme), touch driver,
 * LVGL input registration, touch calibration (load from NVS and run dialog), and SD
 * card over SDSPI with retry scheduling. Ends by seeding in-memory settings state.
 *
 * @note Call once at startup before launching UI tasks.
 */
void starting_routine(void);

/**
 * @brief Open the Settings UI, creating it on first call and loading it into LVGL.
 *
 * @param return_screen Screen to switch back to when closing settings (nullable).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad return screen
 */
esp_err_t settings_open_settings(lv_obj_t *return_screen);

/**
 * @brief Show the Set Date/Time dialog.
 *
 * Builds the date/time picker overlay on the top layer. Caller provides the
 * screen to return focus to (used for context). The dialog uses the shared
 * settings context/state.
 *
 * @param return_screen Screen that owns the caller UI (can be NULL).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if LVGL is not ready.
 */
esp_err_t settings_show_date_time_dialog(lv_obj_t *return_screen);

/**
 * @brief Register callbacks for time set/reset events.
 *
 * @param on_time_set   Called after a successful Apply in the date/time dialog.
 * @param on_time_reset Called when settings are reset (to clear clock UI).
 */
void settings_register_time_callbacks(void (*on_time_set)(void),
                                      void (*on_time_reset)(void));

/**
 * @brief persists current system time to NVS.
 */                                     
void settings_shutdown_save_time(void);     

/**
 * @brief Check if there is any valid value in NVS for system time.
 *
 * @return true if time is valid, false otherwise.
 */
bool settings_is_time_valid();

/**
 * @brief Fade brightness to saved_brightness over SETTINGS_UP_FADE_MS.
 */
void settings_fade_to_saved_brightness(void);

void settings_start_screensaver_timers(void);
bool settings_is_wake_in_progress(void);
int settings_get_active_brightness(void);
bool settings_get_brightness_state(void);

#ifdef __cplusplus
}
#endif
