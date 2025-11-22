#include "jpg.h"

#include <stdbool.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"

#include "jpg_handler.h"

#define TAG "jpg_viewer"
#define IMG_VIEWER_MAX_PATH 256

typedef struct {
    bool active;
    lv_obj_t *screen;
    lv_obj_t *image;
    lv_obj_t *path_label;
    lv_obj_t *return_screen;
    lv_obj_t *previous_screen;
    char path[IMG_VIEWER_MAX_PATH];
} jpg_viewer_ctx_t;

static jpg_viewer_ctx_t s_jpg_viewer;

static void jpg_viewer_reset(jpg_viewer_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
}

static void jpg_viewer_destroy_active(jpg_viewer_ctx_t *ctx)
{
    if (!ctx || !ctx->active) {
        jpg_viewer_reset(ctx);
        return;
    }

    if (bsp_display_lock(0)) {
        if (ctx->screen) {
            lv_obj_del(ctx->screen);
        }
        bsp_display_unlock();
    }

    jpg_viewer_reset(ctx);
}

static void jpg_viewer_on_close(lv_event_t *e)
{
    jpg_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->active) {
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }

    lv_obj_t *old_screen = ctx->screen;
    lv_obj_t *target = ctx->return_screen ? ctx->return_screen : ctx->previous_screen;
    if (target) {
        lv_screen_load(target);
    }

    if (old_screen) {
        lv_obj_del(old_screen);
    }

    bsp_display_unlock();
    jpg_viewer_reset(ctx);
}

static void jpg_viewer_build_ui(jpg_viewer_ctx_t *ctx, const char *path)
{
    ctx->screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ctx->screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ctx->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ctx->screen, 8, 0);

    /* Header row with path and close button */
    lv_obj_t *header = lv_obj_create(ctx->screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_set_style_pad_row(header, 0, 0);
    lv_obj_set_style_pad_column(header, 4, 0);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ctx->path_label = lv_label_create(header);
    lv_label_set_long_mode(ctx->path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ctx->path_label, lv_pct(80));
    lv_label_set_text(ctx->path_label, path);
    lv_obj_set_style_text_color(ctx->path_label, lv_color_hex(0xffffff), 0);

    lv_obj_t *close_btn = lv_button_create(header);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, jpg_viewer_on_close, LV_EVENT_CLICKED, ctx);

    ctx->image = lv_image_create(ctx->screen);
    lv_obj_center(ctx->image);
}

esp_err_t jpg_viewer_open(const jpg_viewer_open_opts_t *opts)
{
    if (!opts || !opts->path || opts->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    jpg_viewer_ctx_t *ctx = &s_jpg_viewer;

    if (ctx->active) {
        jpg_viewer_destroy_active(ctx);
    }

    ctx->return_screen = opts->return_screen;
    strlcpy(ctx->path, opts->path, sizeof(ctx->path));

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    ctx->previous_screen = lv_screen_active();
    jpg_viewer_build_ui(ctx, opts->path);

    esp_err_t err = jpg_handler_set_src(ctx->image, opts->path);
    if (err != ESP_OK) {
        lv_obj_del(ctx->screen);
        ctx->screen = NULL;
        bsp_display_unlock();
        jpg_viewer_reset(ctx);
        return err;
    }

    lv_obj_center(ctx->image);
    lv_screen_load(ctx->screen);
    bsp_display_unlock();

    ctx->active = true;
    return ESP_OK;
}
