#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ui.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

// Displays a white screen with centered text for `ms` milliseconds
void ui_show_message(const char *txt)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);

    lv_obj_update_layout(scr);
    lv_refr_now(NULL);
}

typedef struct
{
    SemaphoreHandle_t xSemaphore;
    bool resonse;
} msg_box_response_t;

static void event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    msg_box_response_t *msg_respone = (msg_box_response_t *)lv_event_get_user_data(e);

    if (strcmp(lv_label_get_text(label), "Yes") == 0)
    {
        msg_respone->resonse = true;
    }
    else if (strcmp(lv_label_get_text(label), "No") == 0)
    {
        msg_respone->resonse = false;
    }

    xSemaphoreGive(msg_respone->xSemaphore);
}

bool ui_yes_no_dialog(const char *question)
{
    bsp_display_lock(0); /* Take LVGL Mutex */

    lv_obj_t *mbox1 = lv_msgbox_create(NULL);

    lv_obj_t *label = lv_label_create(mbox1);
    lv_label_set_text(label, question);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));

    msg_box_response_t msg_box_response = {
        .xSemaphore = xSemaphoreCreateBinary(),
        .resonse = false};

    lv_obj_t *btn;
    btn = lv_msgbox_add_footer_button(mbox1, "Yes");
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, &msg_box_response);
    btn = lv_msgbox_add_footer_button(mbox1, "No");
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, &msg_box_response);

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_disp_get_default());

    bsp_display_unlock(); /* Give LVGL Mutex */

    while (xSemaphoreTake(msg_box_response.xSemaphore, pdMS_TO_TICKS(10)) == pdFALSE)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    bsp_display_lock(0); /* Take LVGL Mutex */
    lv_msgbox_close(mbox1);
    bsp_display_unlock(); /* Give LVGL Mutex */

    return msg_box_response.resonse;
}

// Helper: create a line with a given style on a parent
static void make_line(lv_obj_t *parent, const lv_point_precise_t *pts, uint16_t cnt, const lv_style_t *style)
{
    lv_obj_t *l = lv_line_create(parent);
    if (style)
        lv_obj_add_style(l, style, 0);
    lv_line_set_points(l, pts, cnt);
}

// Draw a 4-arrow target pointing to (x,y) on a white background (LVGL v9)
void draw_cross(int x, int y)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); // remove old children

    // shared line style (black, width 3)
    static lv_style_t st;
    static bool inited = false;
    if (!inited)
    {
        lv_style_init(&st);
        lv_style_set_line_width(&st, 3);
        lv_style_set_line_color(&st, lv_color_black());
        lv_style_set_line_rounded(&st, false);
        inited = true;
    }

    const int gap = 5;  // tip distance from center
    const int len = 24; // shaft length
    const int head = 7; // arrow head size

    // ---- Up arrow (points down toward center)
    lv_point_precise_t up_shaft[] = {{x, y - gap - len}, {x, y - gap}};
    lv_point_precise_t up_head_l[] = {{x, y - gap}, {x - head, y - gap - head}};
    lv_point_precise_t up_head_r[] = {{x, y - gap}, {x + head, y - gap - head}};
    make_line(scr, up_shaft, 2, &st);
    make_line(scr, up_head_l, 2, &st);
    make_line(scr, up_head_r, 2, &st);

    // ---- Down arrow (points up toward center)
    lv_point_precise_t down_shaft[] = {{x, y + gap + len}, {x, y + gap}};
    lv_point_precise_t down_head_l[] = {{x, y + gap}, {x - head, y + gap + head}};
    lv_point_precise_t down_head_r[] = {{x, y + gap}, {x + head, y + gap + head}};
    make_line(scr, down_shaft, 2, &st);
    make_line(scr, down_head_l, 2, &st);
    make_line(scr, down_head_r, 2, &st);

    // ---- Left arrow (points right toward center)
    lv_point_precise_t left_shaft[] = {{x - gap - len, y}, {x - gap, y}};
    lv_point_precise_t left_head_u[] = {{x - gap, y}, {x - gap - head, y - head}};
    lv_point_precise_t left_head_d[] = {{x - gap, y}, {x - gap - head, y + head}};
    make_line(scr, left_shaft, 2, &st);
    make_line(scr, left_head_u, 2, &st);
    make_line(scr, left_head_d, 2, &st);

    // ---- Right arrow (points left toward center)
    lv_point_precise_t right_shaft[] = {{x + gap + len, y}, {x + gap, y}};
    lv_point_precise_t right_head_u[] = {{x + gap, y}, {x + gap + head, y - head}};
    lv_point_precise_t right_head_d[] = {{x + gap, y}, {x + gap + head, y + head}};
    make_line(scr, right_shaft, 2, &st);
    make_line(scr, right_head_u, 2, &st);
    make_line(scr, right_head_d, 2, &st);

    lv_refr_now(NULL);
}
