// main/file_browser.c
#include "file_browser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fs_navigator.h"
#include "esp_vfs_fat.h"
#include "lvgl.h"

#include "sd_protocol_types.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#define TAG "file_browser"

#define FILE_BROWSER_MAX_ENTRIES_DEFAULT 512

typedef struct {
    bool initialized;
    fs_nav_t nav;
    lv_obj_t *screen;
    lv_obj_t *path_label;
    lv_obj_t *sort_mode_label;
    lv_obj_t *sort_dir_label;
    lv_obj_t *list;
} file_browser_ctx_t;

static file_browser_ctx_t s_browser;

static void file_browser_build_screen(file_browser_ctx_t *ctx);
static void file_browser_sync_view(file_browser_ctx_t *ctx);
static void file_browser_update_path_label(file_browser_ctx_t *ctx);
static void file_browser_update_sort_badges(file_browser_ctx_t *ctx);
static void file_browser_populate_list(file_browser_ctx_t *ctx);
static const char *file_browser_sort_mode_text(fs_nav_sort_mode_t mode);
static void file_browser_show_error_screen(const char *root_path, esp_err_t err);
static void file_browser_format_size(size_t bytes, char *out, size_t out_len);

static void file_browser_on_entry_click(lv_event_t *e);
static void file_browser_on_parent_click(lv_event_t *e);
static void file_browser_on_sort_mode_click(lv_event_t *e);
static void file_browser_on_sort_dir_click(lv_event_t *e);

static sdmmc_card_t *s_sd_card = NULL;
static bool s_spi_bus_ready = false;

void init_sdspi(void)
{
    if (!s_spi_bus_ready) {
        spi_bus_config_t spi_bus_config = {
            .mosi_io_num = SDSPI_BUS_MOSI_PIN,
            .miso_io_num = SDSPI_BUS_MISO_PIN,
            .sclk_io_num = SPSPI_BUS_SCL_PIN,
            .max_transfer_sz = 4096,
        };
        esp_err_t bus_err = spi_bus_initialize(SDSPI_BUS_HOST, &spi_bus_config, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to init SDSPI bus: %s", esp_err_to_name(bus_err));
            ESP_ERROR_CHECK(bus_err);
        }
        s_spi_bus_ready = true;
    }

    if (s_sd_card) {
        ESP_LOGI(TAG, "SDSPI already mounted at %s", SDSPI_MOUNT_POINT);
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDSPI_BUS_HOST;
    host.max_freq_khz = SDMMC_HOST_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SDSPI_BUS_HOST;
    slot_config.gpio_cs = SDSPI_DEVICE_CS_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SDSPI filesystem at %s", SDSPI_MOUNT_POINT);
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SDSPI_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card (%s). Check wiring/pull-ups.", esp_err_to_name(ret));
        }
        ESP_ERROR_CHECK(ret);
    }

    sdmmc_card_print_info(stdout, s_sd_card);
    ESP_LOGI(TAG, "SDSPI ready");
}

esp_err_t file_browser_start(const file_browser_config_t *cfg)
{
    if (!cfg || !cfg->root_path) {
        return ESP_ERR_INVALID_ARG;
    }

    file_browser_ctx_t *ctx = &s_browser;
    memset(ctx, 0, sizeof(*ctx));

    fs_nav_config_t nav_cfg = {
        .root_path = cfg->root_path,
        .max_entries = cfg->max_entries ? cfg->max_entries : FILE_BROWSER_MAX_ENTRIES_DEFAULT,
    };

    esp_err_t nav_err = fs_nav_init(&ctx->nav, &nav_cfg);
    if (nav_err != ESP_OK) {
        file_browser_show_error_screen(cfg->root_path, nav_err);
        return nav_err;
    }
    ctx->initialized = true;

    if (!bsp_display_lock(0)) {
        fs_nav_deinit(&ctx->nav);
        ctx->initialized = false;
        return ESP_ERR_TIMEOUT;
    }

    file_browser_build_screen(ctx);
    file_browser_sync_view(ctx);
    lv_screen_load(ctx->screen);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t file_browser_reload(void)
{
    file_browser_ctx_t *ctx = &s_browser;
    if (!ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = fs_nav_refresh(&ctx->nav);
    if (err != ESP_OK) {
        return err;
    }

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    file_browser_sync_view(ctx);
    bsp_display_unlock();
    return ESP_OK;
}

static void file_browser_build_screen(file_browser_ctx_t *ctx)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101218), 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_gap(scr, 8, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    ctx->screen = scr;

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(header, 8, 0);

    ctx->path_label = lv_label_create(header);
    lv_label_set_long_mode(ctx->path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(ctx->path_label, 1);
    lv_label_set_text(ctx->path_label, "-");

    lv_obj_t *sort_mode_btn = lv_button_create(header);
    lv_obj_set_style_radius(sort_mode_btn, 6, 0);
    lv_obj_set_style_pad_all(sort_mode_btn, 6, 0);
    lv_obj_add_event_cb(sort_mode_btn, file_browser_on_sort_mode_click, LV_EVENT_CLICKED, ctx);
    ctx->sort_mode_label = lv_label_create(sort_mode_btn);
    lv_label_set_text(ctx->sort_mode_label, "Name");

    lv_obj_t *sort_dir_btn = lv_button_create(header);
    lv_obj_set_style_radius(sort_dir_btn, 6, 0);
    lv_obj_set_style_pad_all(sort_dir_btn, 6, 0);
    lv_obj_add_event_cb(sort_dir_btn, file_browser_on_sort_dir_click, LV_EVENT_CLICKED, ctx);
    ctx->sort_dir_label = lv_label_create(sort_dir_btn);
    lv_label_set_text(ctx->sort_dir_label, LV_SYMBOL_UP);

    ctx->list = lv_list_create(scr);
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_size(ctx->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(ctx->list, 0, 0);
}

static void file_browser_sync_view(file_browser_ctx_t *ctx)
{
    if (!ctx->screen) {
        return;
    }
    file_browser_update_path_label(ctx);
    file_browser_update_sort_badges(ctx);
    file_browser_populate_list(ctx);
}

static void file_browser_update_path_label(file_browser_ctx_t *ctx)
{
    const char *path = fs_nav_current_path(&ctx->nav);
    lv_label_set_text(ctx->path_label, path ? path : "-");
}

static void file_browser_update_sort_badges(file_browser_ctx_t *ctx)
{
    lv_label_set_text(ctx->sort_mode_label, file_browser_sort_mode_text(fs_nav_get_sort(&ctx->nav)));
    lv_label_set_text(ctx->sort_dir_label, fs_nav_is_sort_ascending(&ctx->nav) ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

static void file_browser_populate_list(file_browser_ctx_t *ctx)
{
    lv_obj_clean(ctx->list);

    if (fs_nav_can_go_parent(&ctx->nav)) {
        lv_obj_t *parent_btn = lv_list_add_btn(ctx->list, LV_SYMBOL_UP, ".. (Parent)");
        lv_obj_add_event_cb(parent_btn, file_browser_on_parent_click, LV_EVENT_CLICKED, ctx);
    }

    size_t count = 0;
    const fs_nav_entry_t *entries = fs_nav_entries(&ctx->nav, &count);
    if (!entries || count == 0) {
        lv_obj_t *lbl = lv_label_create(ctx->list);
        lv_label_set_text(lbl, "Empty folder");
        lv_obj_center(lbl);
        lv_obj_set_style_text_opa(lbl, LV_OPA_60, 0);
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        const fs_nav_entry_t *entry = &entries[i];
        char meta[32];
        if (entry->is_dir) {
            strlcpy(meta, "Folder", sizeof(meta));
        } else {
            file_browser_format_size(entry->size_bytes, meta, sizeof(meta));
        }

        char text[FS_NAV_MAX_NAME + 48];
        snprintf(text, sizeof(text), "%s\n%s", entry->name, meta);

        lv_obj_t *btn = lv_list_add_btn(ctx->list,
                                        entry->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                                        text);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, file_browser_on_entry_click, LV_EVENT_CLICKED, ctx);
    }
}

static const char *file_browser_sort_mode_text(fs_nav_sort_mode_t mode)
{
    switch (mode) {
        case FS_NAV_SORT_DATE:
            return "Date";
        case FS_NAV_SORT_SIZE:
            return "Size";
        case FS_NAV_SORT_NAME:
        default:
            return "Name";
    }
}

static void file_browser_format_size(size_t bytes, char *out, size_t out_len)
{
    static const char *suffixes[] = {"B", "KB", "MB", "GB"};
    double value = (double)bytes;
    size_t idx = 0;
    while (value >= 1024.0 && idx < 3) {
        value /= 1024.0;
        idx++;
    }
    if (idx == 0) {
        snprintf(out, out_len, "%u %s", (unsigned int)bytes, suffixes[idx]);
    } else {
        snprintf(out, out_len, "%.1f %s", value, suffixes[idx]);
    }
}

static void file_browser_on_entry_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    lv_obj_t *btn = lv_event_get_target(e);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(btn);

    size_t count = 0;
    const fs_nav_entry_t *entries = fs_nav_entries(&ctx->nav, &count);
    if (!entries || index >= count) {
        return;
    }

    const fs_nav_entry_t *entry = &entries[index];
    if (entry->is_dir) {
        esp_err_t err = fs_nav_enter(&ctx->nav, index);
        if (err == ESP_OK) {
            file_browser_sync_view(ctx);
        } else {
            ESP_LOGE(TAG, "Failed to enter \"%s\": %s", entry->name, esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "File selected: %s (%zu bytes)", entry->name, entry->size_bytes);
    }
}

static void file_browser_on_parent_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    esp_err_t err = fs_nav_go_parent(&ctx->nav);
    if (err == ESP_OK) {
        file_browser_sync_view(ctx);
    } else {
        ESP_LOGE(TAG, "Failed to go parent: %s", esp_err_to_name(err));
    }
}

static void file_browser_on_sort_mode_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    fs_nav_sort_mode_t mode = fs_nav_get_sort(&ctx->nav);
    mode = (mode + 1) % FS_NAV_SORT_COUNT;

    if (fs_nav_set_sort(&ctx->nav, mode, fs_nav_is_sort_ascending(&ctx->nav)) == ESP_OK) {
        file_browser_update_sort_badges(ctx);
        file_browser_populate_list(ctx);
    }
}

static void file_browser_on_sort_dir_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    bool ascending = fs_nav_is_sort_ascending(&ctx->nav);
    if (fs_nav_set_sort(&ctx->nav, fs_nav_get_sort(&ctx->nav), !ascending) == ESP_OK) {
        file_browser_update_sort_badges(ctx);
        file_browser_populate_list(ctx);
    }
}

static void file_browser_show_error_screen(const char *root_path, esp_err_t err)
{
    if (!bsp_display_lock(0)) {
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x120a0a), 0);
    lv_obj_center(scr);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text_fmt(label,
                          "Storage not ready\nPath: %s\n(%s)",
                          root_path,
                          esp_err_to_name(err));
    lv_obj_center(label);

    lv_screen_load(scr);
    bsp_display_unlock();
}
