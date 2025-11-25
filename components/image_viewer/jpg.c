#include "jpg.h"

#include <stdbool.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/misc/lv_fs.h"

#define TAG "jpg_viewer"
#define IMG_VIEWER_MAX_PATH 256

typedef struct {
    lv_fs_file_t file;
    esp_lcd_panel_handle_t panel;
    uint16_t *stripe;          /* DMA-capable stripe buffer */
    uint32_t stripe_w;
    uint32_t stripe_h;
} jpg_stripe_ctx_t;

typedef struct {
    bool active;
    lv_obj_t *screen;
    lv_obj_t *image;
    lv_obj_t *close_btn;
    lv_obj_t *path_label;
    lv_obj_t *return_screen;
    lv_obj_t *previous_screen;
    lv_timer_t *dim_timer;
    char path[IMG_VIEWER_MAX_PATH];
} jpg_viewer_ctx_t;

static jpg_viewer_ctx_t s_jpg_viewer;

/**
 * @brief Set the LVGL image source to a JPEG on disk.
 *
 * Performs basic validation (non-empty path) before calling jpg_draw_striped().
 *
 * @param img  LVGL image object (must be non-NULL).
 * @param path Path to JPEG file (drive-prefixed if using LVGL FS, e.g. "S:/...").
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad inputs.
 */
static esp_err_t jpg_handler_set_src(lv_obj_t *img, const char *path);
static esp_err_t jpg_draw_striped(const char *path, esp_lcd_panel_handle_t panel);
static size_t input_cb(JDEC *jd, uint8_t *buff, size_t nbytes);
static int output_cb(JDEC *jd, void *bitmap, JRECT *rect);

static void jpg_viewer_destroy_active(jpg_viewer_ctx_t *ctx);
static void jpg_viewer_build_ui(jpg_viewer_ctx_t *ctx, const char *path);
static void jpg_viewer_reset(jpg_viewer_ctx_t *ctx);
static void jpg_viewer_on_close(lv_event_t *e);
static void jpg_viewer_on_screen_tap(lv_event_t *e);
static void jpg_viewer_start_dim_timer(jpg_viewer_ctx_t *ctx);
static void jpg_viewer_dim_cb(lv_timer_t *timer);
static void jpg_viewer_set_close_opacity(jpg_viewer_ctx_t *ctx, lv_opa_t opa);
static void jpg_viewer_restore_close_opacity(jpg_viewer_ctx_t *ctx);
static void jpg_viewer_anim_set_btn_opa(void *obj, int32_t v);

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

    /* Load the screen before drawing so LVGL flushes its background/UI first */
    lv_screen_load(ctx->screen);
    /* Force a refresh now so subsequent LVGL cycles don't clear our direct draw */
    lv_refr_now(NULL);

    ESP_LOGW("", "Before jpg_handler_set_src");

    esp_err_t err = jpg_handler_set_src(ctx->image, opts->path);
    if (err != ESP_OK) {
        if (ctx->previous_screen) {
            lv_screen_load(ctx->previous_screen);
        }
        lv_obj_del(ctx->screen);
        ctx->screen = NULL;
        bsp_display_unlock();
        jpg_viewer_reset(ctx);
        return err;
    }

    ESP_LOGW("", "After jpg_handler_set_src");

    jpg_viewer_restore_close_opacity(ctx);

    bsp_display_unlock();

    ctx->active = true;
    return ESP_OK;
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

static void jpg_viewer_build_ui(jpg_viewer_ctx_t *ctx, const char *path)
{
    ctx->screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ctx->screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ctx->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ctx->screen, 0, 0);
    lv_obj_add_flag(ctx->screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ctx->screen, jpg_viewer_on_screen_tap, LV_EVENT_CLICKED, ctx);

    ctx->image = lv_image_create(ctx->screen);
    lv_obj_center(ctx->image);

    lv_obj_t *close_btn = lv_button_create(ctx->screen);
    ctx->close_btn = close_btn;
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(close_btn, jpg_viewer_on_close, LV_EVENT_CLICKED, ctx);
}

static esp_err_t jpg_handler_set_src(lv_obj_t *img, const char *path)
{
    if (!img || !path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    if (!panel) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW("", "Before jpg_draw_striped");
    esp_err_t err = jpg_draw_striped(path, panel);
    ESP_LOGW("", "After jpg_draw_striped");

    return err;
}

static void jpg_viewer_reset(jpg_viewer_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->dim_timer) {
        lv_timer_del(ctx->dim_timer);
        ctx->dim_timer = NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
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

static void jpg_viewer_on_screen_tap(lv_event_t *e)
{
    jpg_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    jpg_viewer_restore_close_opacity(ctx);
}

static void jpg_viewer_start_dim_timer(jpg_viewer_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->dim_timer) {
        lv_timer_reset(ctx->dim_timer);
    } else {
        ctx->dim_timer = lv_timer_create(jpg_viewer_dim_cb, 3000, ctx);
    }
}

static void jpg_viewer_dim_cb(lv_timer_t *timer)
{
    jpg_viewer_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (!ctx) {
        return;
    }
    jpg_viewer_set_close_opacity(ctx, LV_OPA_20);
}

static void jpg_viewer_set_close_opacity(jpg_viewer_ctx_t *ctx, lv_opa_t opa)
{
    if (!ctx || !ctx->close_btn) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx->close_btn);
    lv_anim_set_values(&a, lv_obj_get_style_opa(ctx->close_btn, LV_PART_MAIN), opa);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, jpg_viewer_anim_set_btn_opa);
    lv_anim_start(&a);
}

static void jpg_viewer_restore_close_opacity(jpg_viewer_ctx_t *ctx)
{
    jpg_viewer_set_close_opacity(ctx, LV_OPA_COVER);
    jpg_viewer_start_dim_timer(ctx);
}

static void jpg_viewer_anim_set_btn_opa(void *obj, int32_t v)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static size_t input_cb(JDEC *jd, uint8_t *buff, size_t nbytes)
{
    jpg_stripe_ctx_t *ctx = (jpg_stripe_ctx_t *)jd->device;
    if (!ctx) {
        return 0;
    }
    lv_fs_file_t *f = &ctx->file;

    if (buff) {
        uint32_t rn = 0;
        lv_fs_res_t res = lv_fs_read(f, buff, (uint32_t)nbytes, &rn);
        return (res == LV_FS_RES_OK) ? rn : 0;
    }

    uint32_t pos = 0;
    if (lv_fs_tell(f, &pos) != LV_FS_RES_OK) {
        return 0;
    }
    if (lv_fs_seek(f, (uint32_t)(pos + nbytes), LV_FS_SEEK_SET) != LV_FS_RES_OK) {
        return 0;
    }
    return nbytes;
}

static int output_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_stripe_ctx_t *ctx = (jpg_stripe_ctx_t *)jd->device; /* user context */
    if (!ctx || !bitmap || !rect) {
        return 0;
    }

    const int w = rect->right - rect->left + 1;
    const int h = rect->bottom - rect->top + 1;

    /* Ensure stripe buffer is large enough */
    if ((uint32_t)w > ctx->stripe_w || (uint32_t)h > ctx->stripe_h) {
        return 0;
    }

    uint8_t *src = (uint8_t *)bitmap; /* RGB888 from tjpgd (JD_FORMAT=0) */
    uint16_t *dst = ctx->stripe;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;
            /* Panel is configured BGR; swap R and B, then pack RGB565 and swap bytes */
            uint8_t b = src[idx];
            uint8_t g = src[idx + 1];
            uint8_t r = src[idx + 2];
            uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            c = (uint16_t)((c >> 8) | (c << 8));
            dst[y * w + x] = c;
        }
    }

    esp_lcd_panel_draw_bitmap(ctx->panel,
                              rect->left, rect->top,
                              rect->right + 1, rect->bottom + 1,
                              dst);
    return 1; /* continue */
}

static esp_err_t jpg_draw_striped(const char *path, esp_lcd_panel_handle_t panel)
{
    esp_err_t err = ESP_OK;
    jpg_stripe_ctx_t ctx = {
        .panel = panel,
        .stripe = NULL,
        .stripe_w = 0,
        .stripe_h = 0,
    };

    lv_fs_res_t res = lv_fs_open(&ctx.file, path, LV_FS_MODE_RD);
    if (res != LV_FS_RES_OK) {
        return ESP_FAIL;
    }

    uint8_t workb[4096];      /* tjpgd work buffer */

    JDEC jd;
    JRESULT rc = jd_prepare(&jd, input_cb, workb, sizeof(workb), &ctx);
    if (rc != JDR_OK) {
        err = ESP_FAIL;
        goto cleanup;
    }

    /* MCU height = msy * 8 lines; width can be up to image width */
    ctx.stripe_w = jd.width;
    ctx.stripe_h = jd.msy * 8;
    size_t stripe_size = ctx.stripe_w * ctx.stripe_h * sizeof(uint16_t);
    ctx.stripe = heap_caps_malloc(stripe_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ctx.stripe) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    rc = jd_decomp(&jd, output_cb, 0); /* scale 0 = full res */
    if (rc != JDR_OK) {
        err = ESP_FAIL;
    }

cleanup:
    lv_fs_close(&ctx.file);
    if (ctx.stripe) {
        free(ctx.stripe);
    }
    return err;
}
