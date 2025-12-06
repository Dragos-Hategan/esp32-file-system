#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#define UI_COLOR_BG             lv_color_hex(0x101214)
#define UI_COLOR_CARD           lv_color_hex(0x202327)
#define UI_COLOR_BORDER         lv_color_hex(0x2D3034)
#define UI_COLOR_BUTTON_BORDER  lv_color_hex(0xBBAAFF)
#define UI_COLOR_TEXT           lv_color_hex(0xDCDCDC)
#define UI_COLOR_ACCENT_BLUE    lv_color_hex(0x7D5FFF)
#define UI_COLOR_ACCENT_BLUE_2  lv_color_hex(0x347AFF)
#define UI_COLOR_ACCENT_GREEN   lv_color_hex(0x37B24D)


void styles_build_button(lv_obj_t *button);
void styles_build_msgbox(lv_obj_t *mbox);
void styles_build_keyboard(lv_obj_t *kbd);


#ifdef __cplusplus
}
#endif