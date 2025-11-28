#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void init_settings(void);

/**
 * @brief Rotates the display in 90° increments.
 *
 * This function cycles the screen orientation through 0°, 90°, 180°, and 270°.
 * The current rotation step is tracked in the context and updated on each call.
 *
 * @param ctx Pointer to the file browser context containing rotation state.
 */
void settings_rotate_screen(void);

#ifdef __cplusplus
}
#endif
