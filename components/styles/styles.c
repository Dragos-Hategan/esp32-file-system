#include "styles.h"

void styles_build_button(lv_obj_t *button)
{
    if (!button) {
        return;
    }
    lv_obj_set_style_bg_color(button, UI_COLOR_ACCENT_BLUE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, UI_COLOR_BUTTON_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, UI_COLOR_TEXT, LV_PART_MAIN);
}

void styles_build_msgbox(lv_obj_t *mbox)
{
    if (!mbox) {
        return;
    }
    lv_obj_set_style_bg_color(mbox, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 1, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, UI_COLOR_TEXT, LV_PART_ITEMS);
}

void styles_build_keyboard(lv_obj_t *kbd)
{
    if (!kbd) {
        return;
    }
    lv_obj_set_style_bg_color(kbd, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(kbd, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kbd, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(kbd, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(kbd, UI_COLOR_TEXT, LV_PART_MAIN);

    /* Keys */
    lv_obj_set_style_bg_color(kbd, UI_COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kbd, UI_COLOR_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kbd, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kbd, UI_COLOR_TEXT, LV_PART_ITEMS);

    /* Pressed/checked/focused keys: subtle dark instead of accent */
    lv_obj_set_style_bg_color(kbd, UI_COLOR_BORDER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kbd, UI_COLOR_BORDER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kbd, UI_COLOR_BORDER, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(kbd, UI_COLOR_TEXT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kbd, UI_COLOR_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kbd, UI_COLOR_TEXT, LV_PART_ITEMS | LV_STATE_FOCUSED);
}