#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ui.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

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

/**
 * @brief Response container passed to button event callbacks of the Yes/No dialog.
 */
typedef struct
{
    SemaphoreHandle_t xSemaphore;
    bool resonse;
} msg_box_response_t;

/**
 * @brief LVGL button click event handler for the Yes/No dialog.
 *
 * Interprets the clicked button by reading its first child label’s text.
 * Sets the @ref msg_box_response_t::resonse flag accordingly and gives
 * the semaphore to wake the waiting task.
 *
 * @param e LVGL event descriptor (expects LV_EVENT_CLICKED).
 *
 * @pre The event’s user data must be a valid pointer to @ref msg_box_response_t.
 * @pre The target object must be a button whose first child is a label with text "Yes" or "No".
 *
 * @note Static helper; not intended to be called directly outside this module.
 */
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
    bsp_display_lock(0); 

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *mbox1 = lv_msgbox_create(scr);

    lv_obj_set_style_max_width(mbox1, lv_pct(90), 0);
    lv_obj_align(mbox1, LV_ALIGN_CENTER, 0, -50);    

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

    /* Compact container aligned exactly under the dialog */
    lv_obj_t *loader_wrap = lv_obj_create(scr);
    lv_obj_remove_style_all(loader_wrap);
    lv_obj_set_style_pad_all(loader_wrap, 0, 0);
    lv_obj_set_style_border_width(loader_wrap, 0, 0);
    lv_obj_set_width(loader_wrap, 120);
    lv_obj_set_height(loader_wrap, LV_SIZE_CONTENT);
    lv_obj_align_to(loader_wrap, mbox1, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    lv_obj_t *performing_label = lv_label_create(loader_wrap);
    lv_label_set_text(performing_label, "Performing Calibration");
    lv_obj_set_style_text_align(performing_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(performing_label, lv_pct(100));
    lv_obj_align(performing_label, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *loading_arc = lv_arc_create(loader_wrap);
    lv_obj_set_size(loading_arc, 60, 60);                 // slightly smaller for better proportions
    lv_arc_set_range(loading_arc, 0, 100);
    lv_arc_set_bg_angles(loading_arc, 0, 360);
    lv_arc_set_rotation(loading_arc, 270);
    lv_arc_set_value(loading_arc, 100);
    lv_obj_remove_style(loading_arc, NULL, LV_PART_KNOB);
    lv_obj_align_to(loading_arc, performing_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);    

    lv_obj_t *countdown_label = lv_label_create(loading_arc);
    lv_obj_set_style_text_font(countdown_label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(countdown_label, "5");
    lv_obj_center(countdown_label);

    lv_obj_invalidate(scr);
    lv_refr_now(lv_disp_get_default());

    bsp_display_unlock();

    const uint32_t countdown_ms = 5000;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(countdown_ms);
    int last_second_displayed = 5;
    int last_arc_value = 100;

    while (true) {
        if (xSemaphoreTake(msg_box_response.xSemaphore, pdMS_TO_TICKS(50)) == pdTRUE) {
            break;
        }

        TickType_t now_ticks = xTaskGetTickCount();
        TickType_t elapsed_ticks = now_ticks - start_ticks;
        uint32_t elapsed_ms = elapsed_ticks * portTICK_PERIOD_MS;

        if (elapsed_ticks >= timeout_ticks) {
            msg_box_response.resonse = true;
            break;
        }

        int seconds_left = 5 - (elapsed_ms / 1000);
        if (seconds_left < 1) {
            seconds_left = 1;
        }

        int arc_value = 100 - (int)((elapsed_ms * 100) / countdown_ms);
        if (arc_value < 0) arc_value = 0;

        if (seconds_left != last_second_displayed || arc_value != last_arc_value) {
            bsp_display_lock(0);
            if (seconds_left != last_second_displayed) {
                lv_label_set_text_fmt(countdown_label, "%d", seconds_left);
                last_second_displayed = seconds_left;
            }
            if (arc_value != last_arc_value) {
                lv_arc_set_value(loading_arc, arc_value);
                last_arc_value = arc_value;
            }
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    bsp_display_lock(0); 
    lv_msgbox_close(mbox1);
    lv_obj_del(performing_label);
    lv_obj_del(loading_arc);
    bsp_display_unlock(); 

    vSemaphoreDelete(msg_box_response.xSemaphore);

    return msg_box_response.resonse;
}

/**
 * @brief Create an LVGL line object with a given point array and optional style.
 *
 * Convenience wrapper around @c lv_line_create and @c lv_line_set_points.
 *
 * @param parent Parent LVGL object.
 * @param pts    Pointer to an array of points (in screen coordinates).
 * @param cnt    Number of points in @p pts.
 * @param style  Optional style to apply to the line (can be NULL).
 *
 * @note Static helper; not intended to be used outside this module.
 */
static void make_line(lv_obj_t *parent, const lv_point_precise_t *pts, uint16_t cnt, const lv_style_t *style)
{
    lv_obj_t *l = lv_line_create(parent);
    if (style)
        lv_obj_add_style(l, style, 0);
    lv_line_set_points(l, pts, cnt);
}

void draw_cross(int x, int y)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); // remove old children

    /* ---- Shared line style (black, width 3) ----- */
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

    /* ---- Up arrow (points down toward center) ----- */
    lv_point_precise_t up_shaft[] = {{x, y - gap - len}, {x, y - gap}};
    lv_point_precise_t up_head_l[] = {{x, y - gap}, {x - head, y - gap - head}};
    lv_point_precise_t up_head_r[] = {{x, y - gap}, {x + head, y - gap - head}};
    make_line(scr, up_shaft, 2, &st);
    make_line(scr, up_head_l, 2, &st);
    make_line(scr, up_head_r, 2, &st);

    /* ---- Down arrow (points up toward center) ----- */
    lv_point_precise_t down_shaft[] = {{x, y + gap + len}, {x, y + gap}};
    lv_point_precise_t down_head_l[] = {{x, y + gap}, {x - head, y + gap + head}};
    lv_point_precise_t down_head_r[] = {{x, y + gap}, {x + head, y + gap + head}};
    make_line(scr, down_shaft, 2, &st);
    make_line(scr, down_head_l, 2, &st);
    make_line(scr, down_head_r, 2, &st);

    /* ---- Left arrow (points right toward center) ----- */
    lv_point_precise_t left_shaft[] = {{x - gap - len, y}, {x - gap, y}};
    lv_point_precise_t left_head_u[] = {{x - gap, y}, {x - gap - head, y - head}};
    lv_point_precise_t left_head_d[] = {{x - gap, y}, {x - gap - head, y + head}};
    make_line(scr, left_shaft, 2, &st);
    make_line(scr, left_head_u, 2, &st);
    make_line(scr, left_head_d, 2, &st);

    /* ---- Right arrow (points left toward center) ----- */
    lv_point_precise_t right_shaft[] = {{x + gap + len, y}, {x + gap, y}};
    lv_point_precise_t right_head_u[] = {{x + gap, y}, {x + gap + head, y - head}};
    lv_point_precise_t right_head_d[] = {{x + gap, y}, {x + gap + head, y + head}};
    make_line(scr, right_shaft, 2, &st);
    make_line(scr, right_head_u, 2, &st);
    make_line(scr, right_head_d, 2, &st);

    lv_refr_now(NULL);
}
