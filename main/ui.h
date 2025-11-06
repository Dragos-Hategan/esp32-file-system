#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_touch.h"

#ifndef UI_H
#define UI_H

void ui_show_message(const char *txt);
/** 
 * @warning This function assumes there is no LVGL display lock already acquired.
 */
bool ui_yes_no_dialog(const char *question);
void draw_cross(int x, int y);

#endif // UI_H