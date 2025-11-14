#include "file_browser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_types.h"
#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"

#include "fs_navigator.h"
#include "fs_text_ops.h"
#include "text_editor_screen.h"
#include "text_viewer_screen.h"

#define TAG "file_browser"

#define FILE_BROWSER_MAX_ENTRIES_DEFAULT 512

typedef struct {
    bool active;
    bool is_dir;
    bool is_txt;
    char name[FS_NAV_MAX_NAME];
    char directory[FS_NAV_MAX_PATH];
} file_browser_action_entry_t;

typedef enum {
    FILE_BROWSER_ACTION_EDIT = 1,
    FILE_BROWSER_ACTION_DELETE = 2,
    FILE_BROWSER_ACTION_CANCEL = 3,
    FILE_BROWSER_ACTION_RENAME = 4,
} file_browser_action_type_t;

typedef struct {
    bool initialized;
    fs_nav_t nav;
    lv_obj_t *screen;
    lv_obj_t *path_label;
    lv_obj_t *sort_mode_label;
    lv_obj_t *sort_dir_label;
    lv_obj_t *list;
    lv_obj_t *folder_dialog;
    lv_obj_t *folder_textarea;
    lv_obj_t *folder_status_label;
    lv_obj_t *folder_keyboard;
    lv_obj_t *action_mbox;
    lv_obj_t *confirm_mbox;
    lv_obj_t *rename_dialog;
    lv_obj_t *rename_textarea;
    lv_obj_t *rename_status_label;
    lv_obj_t *rename_keyboard;
    file_browser_action_entry_t action_entry;
    bool suppress_click;
} file_browser_ctx_t;

 static file_browser_ctx_t s_browser;

/************************************ UI & Data Refresh Helpers ***********************************/

 static void file_browser_build_screen(file_browser_ctx_t *ctx);
 static void file_browser_sync_view(file_browser_ctx_t *ctx);
 static void file_browser_update_path_label(file_browser_ctx_t *ctx);
 static void file_browser_update_sort_badges(file_browser_ctx_t *ctx);
 static const char *file_browser_sort_mode_text(fs_nav_sort_mode_t mode);
 static void file_browser_populate_list(file_browser_ctx_t *ctx);
 static void file_browser_format_size(size_t bytes, char *out, size_t out_len);
 static void file_browser_show_error_screen(const char *root_path, esp_err_t err);
 static esp_err_t file_browser_reload(void);

/**************************************************************************************************/


/***************************** List Interactions & Text Editor Bridge *****************************/

 static void file_browser_on_entry_click(lv_event_t *e);
 static void file_browser_on_entry_long_press(lv_event_t *e);
 static void file_browser_on_parent_click(lv_event_t *e);
 static void file_browser_on_sort_mode_click(lv_event_t *e);
 static void file_browser_on_sort_dir_click(lv_event_t *e);
 static void file_browser_on_new_txt_click(lv_event_t *e);
 static void file_browser_editor_closed(bool changed, void *user_ctx);

/**************************************************************************************************/


/************************************* Folder Creation Dialog *************************************/

 static void file_browser_on_new_folder_click(lv_event_t *e);
 static void file_browser_show_folder_dialog(file_browser_ctx_t *ctx);
 static void file_browser_close_folder_dialog(file_browser_ctx_t *ctx);
 static void file_browser_on_folder_create(lv_event_t *e);
 static void file_browser_on_folder_cancel(lv_event_t *e);
 static void file_browser_set_folder_status(file_browser_ctx_t *ctx, const char *msg, bool error);
 static esp_err_t file_browser_create_folder(file_browser_ctx_t *ctx, const char *name);

/**************************************************************************************************/


/*********************************** Filesystem Utility Helpers ***********************************/

 static bool file_browser_is_valid_name(const char *name);
 static void file_browser_trim_whitespace(char *name);
 static esp_err_t file_browser_delete_path(const char *path);

/**************************************************************************************************/


/************************************** Action Menu Workflow **************************************/

 static void file_browser_prepare_action_entry(file_browser_ctx_t *ctx, const fs_nav_entry_t *entry);
 static void file_browser_show_action_menu(file_browser_ctx_t *ctx);
 static void file_browser_close_action_menu(file_browser_ctx_t *ctx);
 static void file_browser_on_action_button(lv_event_t *e);
 static void file_browser_show_delete_confirm(file_browser_ctx_t *ctx);
 static void file_browser_close_delete_confirm(file_browser_ctx_t *ctx);
 static void file_browser_on_delete_confirm(lv_event_t *e);
 static esp_err_t file_browser_delete_selected_entry(file_browser_ctx_t *ctx);

/**************************************************************************************************/


/************************************* Action State Utilities *************************************/

 static esp_err_t file_browser_action_compose_path(const file_browser_ctx_t *ctx, char *out, size_t out_len);
 static void file_browser_clear_action_state(file_browser_ctx_t *ctx);

/**************************************************************************************************/


/************************************* Rename Dialog Workflow *************************************/

 static void file_browser_set_rename_status(file_browser_ctx_t *ctx, const char *msg, bool error);
 static void file_browser_show_rename_dialog(file_browser_ctx_t *ctx);
 static void file_browser_close_rename_dialog(file_browser_ctx_t *ctx);
 static void file_browser_on_rename_accept(lv_event_t *e);
 static void file_browser_on_rename_cancel(lv_event_t *e);
 static esp_err_t file_browser_perform_rename(file_browser_ctx_t *ctx, const char *new_name);

/**************************************************************************************************/

static sdmmc_card_t *s_sd_card = NULL;
static bool s_spi_bus_ready = false;

void init_sdspi(void)
{
    if (!s_spi_bus_ready) {
        spi_bus_config_t spi_bus_config = {
            .mosi_io_num = CONFIG_SDSPI_BUS_MOSI_PIN,
            .miso_io_num = CONFIG_SDSPI_BUS_MISO_PIN,
            .sclk_io_num = CONFIG_SPSPI_BUS_SCL_PIN,
            .max_transfer_sz = 4096,
        };
        esp_err_t bus_err = spi_bus_initialize(CONFIG_SDSPI_BUS_HOST, &spi_bus_config, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to init SDSPI bus: %s", esp_err_to_name(bus_err));
            ESP_ERROR_CHECK(bus_err);
        }
        s_spi_bus_ready = true;
    }

    if (s_sd_card) {
        ESP_LOGI(TAG, "SDSPI already mounted at %s", CONFIG_SDSPI_MOUNT_POINT);
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CONFIG_SDSPI_BUS_HOST;
    host.max_freq_khz = CONFIG_SDSPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = CONFIG_SDSPI_BUS_HOST;
    slot_config.gpio_cs = CONFIG_SDSPI_DEVICE_CS_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SDSPI filesystem at %s", CONFIG_SDSPI_MOUNT_POINT);
    esp_err_t ret = esp_vfs_fat_sdspi_mount(CONFIG_SDSPI_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
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

esp_err_t file_browser_start(void)
{
    file_browser_config_t browser_cfg = {
        .root_path = CONFIG_SDSPI_MOUNT_POINT,
        .max_entries = 512,
    };

    if (!browser_cfg.root_path) {
        return ESP_ERR_INVALID_ARG;
    }

    file_browser_ctx_t *ctx = &s_browser;
    memset(ctx, 0, sizeof(*ctx));
    file_browser_clear_action_state(ctx);

    fs_nav_config_t nav_cfg = {
        .root_path = browser_cfg.root_path,
        .max_entries = browser_cfg.max_entries ? browser_cfg.max_entries : FILE_BROWSER_MAX_ENTRIES_DEFAULT,
    };

    esp_err_t nav_err = fs_nav_init(&ctx->nav, &nav_cfg);
    if (nav_err != ESP_OK) {
        file_browser_show_error_screen(browser_cfg.root_path, nav_err);
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

/**
 * @brief Refresh the current directory view and redraw the list.
 *
 * Re-reads directory entries via @c fs_nav_refresh and repopulates the LVGL list.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if the browser was not started
 * - Error from @c fs_nav_refresh
 * - ESP_ERR_TIMEOUT if display lock cannot be acquired
 */
static esp_err_t file_browser_reload(void)
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
    file_browser_clear_action_state(ctx);
    bsp_display_unlock();
    return ESP_OK;
}

/**
 * @brief Build the LVGL screen hierarchy (header + list).
 *
 * Creates the root screen and child widgets (path label, sort buttons, list).
 *
 * @param[in,out] ctx Browser context (must be non-NULL).
 * @internal UI construction only; does not query filesystem.
 */
static void file_browser_build_screen(file_browser_ctx_t *ctx)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    //lv_obj_set_style_bg_color(scr, lv_color_hex(0x101218), 0);
    lv_obj_set_style_pad_all(scr, 5, 0);
    lv_obj_set_style_pad_gap(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    ctx->screen = scr;

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(header, 10, 0);
    /* MIGHT BE CHANGED */
    lv_obj_set_style_bg_color(header, lv_color_hex(0x00ff00), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    /* MIGHT BE CHANGED */

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
    lv_obj_set_style_text_align(ctx->sort_mode_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *sort_dir_btn = lv_button_create(header);
    lv_obj_set_style_radius(sort_dir_btn, 6, 0);
    lv_obj_set_style_pad_all(sort_dir_btn, 6, 0);
    lv_obj_add_event_cb(sort_dir_btn, file_browser_on_sort_dir_click, LV_EVENT_CLICKED, ctx);
    ctx->sort_dir_label = lv_label_create(sort_dir_btn);
    lv_label_set_text(ctx->sort_dir_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_align(ctx->sort_dir_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *new_txt_btn = lv_button_create(header);
    lv_obj_set_style_radius(new_txt_btn, 6, 0);
    lv_obj_set_style_pad_all(new_txt_btn, 6, 0);
    lv_obj_add_event_cb(new_txt_btn, file_browser_on_new_txt_click, LV_EVENT_CLICKED, ctx);
    lv_obj_t *new_lbl = lv_label_create(new_txt_btn);
    lv_label_set_text(new_lbl, "New TXT");
    lv_obj_set_style_text_align(new_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *new_folder_btn = lv_button_create(header);
    lv_obj_set_style_radius(new_folder_btn, 6, 0);
    lv_obj_set_style_pad_all(new_folder_btn, 6, 0);
    lv_obj_add_event_cb(new_folder_btn, file_browser_on_new_folder_click, LV_EVENT_CLICKED, ctx);
    lv_obj_t *new_folder_lbl = lv_label_create(new_folder_btn);
    lv_label_set_text(new_folder_lbl, "New Folder");
    lv_obj_set_style_text_align(new_folder_lbl, LV_TEXT_ALIGN_CENTER, 0);

    ctx->list = lv_list_create(scr);
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_size(ctx->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(ctx->list, 0, 0);
}

/**
 * @brief Synchronize all UI elements with the current navigation state.
 *
 * Updates path, sort badges, and repopulates the list with current entries.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_sync_view(file_browser_ctx_t *ctx)
{
    if (!ctx->screen) {
        return;
    }
    file_browser_update_path_label(ctx);
    file_browser_update_sort_badges(ctx);
    file_browser_populate_list(ctx);
}

/**
 * @brief Update the path label from the current navigator path.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_update_path_label(file_browser_ctx_t *ctx)
{
    const char *path = fs_nav_current_path(&ctx->nav);
    lv_label_set_text(ctx->path_label, path ? path : "-");
}

/**
 * @brief Update the sort mode and direction badges.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_update_sort_badges(file_browser_ctx_t *ctx)
{
    lv_label_set_text(ctx->sort_mode_label, file_browser_sort_mode_text(fs_nav_get_sort(&ctx->nav)));
    lv_label_set_text(ctx->sort_dir_label, fs_nav_is_sort_ascending(&ctx->nav) ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

/**
 * @brief Rebuild the entry list from current directory contents.
 *
 * Adds a parent navigation item (if applicable) and then one button per entry.
 * For files, a formatted size is shown; for directories, "Folder" is shown.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_populate_list(file_browser_ctx_t *ctx)
{
    lv_obj_clean(ctx->list);

    if (fs_nav_can_go_parent(&ctx->nav)) {
        lv_obj_t *parent_btn = lv_list_add_btn(ctx->list, LV_SYMBOL_UP, "../ -> Parent Folder");
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
        lv_obj_add_event_cb(btn, file_browser_on_entry_long_press, LV_EVENT_LONG_PRESSED, ctx);
    }
}

/**
 * @brief Convert a sort mode enum to human-readable text.
 *
 * @param mode Sort mode.
 * @return Constant C-string: "Name", "Date", or "Size".
 */
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

/**
 * @brief Format a byte size into a short human-friendly string.
 *
 * Uses B/KB/MB/GB up to 1 decimal place for KB or larger.
 *
 * @param bytes   Size in bytes.
 * @param[out] out Output buffer for the formatted text.
 * @param out_len Length of @p out.
 */
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

/**
 * @brief Entry click handler: enter directories or log selected file.
 *
 * If the clicked entry is a directory, enters it and refreshes the view.
 * If it is a file, only logs selection.
 *
 * @param e LVGL event (CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_entry_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    if (ctx->suppress_click) {
        ctx->suppress_click = false;
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
        return;
    }

    if (fs_text_is_txt(entry->name)) {
        char path[FS_NAV_MAX_PATH];
        if (fs_nav_compose_path(&ctx->nav, entry->name, path, sizeof(path)) == ESP_OK) {
            text_viewer_open_opts_t opts = {
                .path = path,
                .return_screen = ctx->screen,
                .editable = false,
            };
            esp_err_t err = text_viewer_open(&opts);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to view \"%s\": %s", entry->name, esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Path too long for \"%s\"", entry->name);
        }
        return;
    }

    ESP_LOGI(TAG, "File selected (no handler): %s (%zu bytes)", entry->name, entry->size_bytes);
}

/**
 * @brief Long-press handler for a list entry to open the action menu.
 *
 * Marks the click as suppressed (to avoid triggering the normal click handler),
 * resolves the pressed entry index, prepares the action entry and shows
 * the action menu.
 *
 * @param e LVGL event (LV_EVENT_LONG_PRESSED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_entry_long_press(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    ctx->suppress_click = true;

    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_remove_state(btn, LV_STATE_PRESSED | LV_STATE_FOCUSED);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(btn);

    size_t count = 0;
    const fs_nav_entry_t *entries = fs_nav_entries(&ctx->nav, &count);
    if (!entries || index >= count) {
        return;
    }

    const fs_nav_entry_t *entry = &entries[index];
    file_browser_prepare_action_entry(ctx, entry);
    file_browser_show_action_menu(ctx);
}

/**
 * @brief Parent button handler: go up one level (if possible).
 *
 * @param e LVGL event (CLICKED) with user data = @c file_browser_ctx_t*.
 */
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

/**
 * @brief Callback invoked when the text editor/viewer screen is closed.
 *
 * If the content was changed, triggers a browser reload to reflect updates
 * (file size, timestamp, new file, etc.).
 *
 * @param changed  True if the editor modified the file.
 * @param user_ctx User context, expected to be @c file_browser_ctx_t*.
 */
static void file_browser_editor_closed(bool changed, void *user_ctx)
{
    file_browser_ctx_t *ctx = (file_browser_ctx_t *)user_ctx;
    if (!ctx || !changed) {
        return;
    }

    esp_err_t err = file_browser_reload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload after editor: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Sort-mode button handler: cycle sort mode and refresh list.
 *
 * Order cycles through Name → Date → Size (per @c FS_NAV_SORT_COUNT).
 *
 * @param e LVGL event (CLICKED) with user data = @c file_browser_ctx_t*.
 */
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

/**
 * @brief Sort-direction button handler: toggle ascending/descending and refresh list.
 *
 * @param e LVGL event (CLICKED) with user data = @c file_browser_ctx_t*.
 */
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

/**
 * @brief "New TXT" button handler: open an editor for a new text file.
 *
 * Uses the current navigator path as parent directory and opens the text editor
 * with a suggested default filename. On close, the browser is notified via
 * @c file_browser_editor_closed().
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_new_txt_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    const char *dir = fs_nav_current_path(&ctx->nav);
    if (!dir) {
        return;
    }

    text_editor_open_opts_t opts = {
        .directory = dir,
        .suggested_name = "new_file.txt",
        .return_screen = ctx->screen,
        .on_close = file_browser_editor_closed,
        .user_ctx = ctx,
    };
    esp_err_t err = text_editor_open(&opts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start new file editor: %s", esp_err_to_name(err));
    }
}

/**
 * @brief "New Folder" button handler: show the folder creation dialog.
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_new_folder_click(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    file_browser_show_folder_dialog(ctx);
}

/**
 * @brief Show a minimal error screen when the storage root is not available.
 *
 * Used when @c fs_nav_init fails. Shows path and error name.
 *
 * @param root_path Root path attempted.
 * @param err       Error that occurred.
 * @note Acquires the display lock internally (non-blocking timeout = 0).
 */
static void file_browser_show_error_screen(const char *root_path, esp_err_t err)
{
    if (!bsp_display_lock(0)) {
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    //lv_obj_set_style_bg_color(scr, lv_color_hex(0x120a0a), 0);
    lv_obj_center(scr);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text_fmt(label,
                          "Storage not ready\nPath: %s\n(%s)",
                          root_path,
                          esp_err_to_name(err));

    lv_obj_center(label);

    lv_obj_t *label2 = lv_label_create(scr);
    lv_label_set_text_fmt(label2, "TO DO: FALLBACK");
    lv_obj_align_to(label2, label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_screen_load(scr);
    bsp_display_unlock();
}

/**
 * @brief Show the "Create folder" dialog overlay.
 *
 * Creates a semi-transparent overlay with a card containing title, status label,
 * folder-name text area, action buttons and an on-screen keyboard. The dialog is
 * stored in @c ctx->folder_dialog and related pointers.
 *
 * @param[in,out] ctx Browser context that owns the dialog.
 */
static void file_browser_show_folder_dialog(file_browser_ctx_t *ctx)
{
    if (ctx->folder_dialog) {
        return;
    }

    lv_obj_t *overlay = lv_obj_create(ctx->screen);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_center(overlay);
    ctx->folder_dialog = overlay;

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_gap(card, 10, 0);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Create folder");

    ctx->folder_status_label = lv_label_create(card);
    lv_label_set_text(ctx->folder_status_label, "");

    ctx->folder_textarea = lv_textarea_create(card);
    lv_textarea_set_one_line(ctx->folder_textarea, true);
    lv_textarea_set_placeholder_text(ctx->folder_textarea, "Folder name");
    lv_textarea_set_max_length(ctx->folder_textarea, FS_NAV_MAX_NAME - 1);

    lv_obj_t *buttons = lv_obj_create(card);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(buttons, 8, 0);

    lv_obj_t *create_btn = lv_button_create(buttons);
    lv_obj_t *create_lbl = lv_label_create(create_btn);
    lv_label_set_text(create_lbl, "Create");
    lv_obj_set_style_text_align(create_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(create_btn, file_browser_on_folder_create, LV_EVENT_CLICKED, ctx);

    lv_obj_t *cancel_btn = lv_button_create(buttons);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_align(cancel_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(cancel_btn, file_browser_on_folder_cancel, LV_EVENT_CLICKED, ctx);

    ctx->folder_keyboard = lv_keyboard_create(overlay);
    lv_keyboard_set_textarea(ctx->folder_keyboard, ctx->folder_textarea);
    lv_obj_add_state(ctx->folder_textarea, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ctx->folder_textarea, file_browser_on_folder_create, LV_EVENT_READY, ctx);
}

/**
 * @brief Close and destroy the "Create folder" dialog overlay.
 *
 * Deletes the overlay object and clears all folder dialog-related pointers
 * in the context.
 *
 * @param[in,out] ctx Browser context that owns the dialog.
 */
static void file_browser_close_folder_dialog(file_browser_ctx_t *ctx)
{
    if (!ctx->folder_dialog) {
        return;
    }
    lv_obj_del(ctx->folder_dialog);
    ctx->folder_dialog = NULL;
    ctx->folder_textarea = NULL;
    ctx->folder_status_label = NULL;
    ctx->folder_keyboard = NULL;
}

/**
 * @brief Set status message and color in the "Create folder" dialog.
 *
 * Updates the folder status label text and chooses an error or neutral color
 * depending on the @p error flag.
 *
 * @param[in,out] ctx Browser context owning the folder status label.
 * @param msg         Message text to display (must be non-NULL).
 * @param error       True to use an error color, false for neutral/info color.
 */
static void file_browser_set_folder_status(file_browser_ctx_t *ctx, const char *msg, bool error)
{
    if (!ctx->folder_status_label || !msg) {
        return;
    }
    lv_obj_set_style_text_color(ctx->folder_status_label,
                                error ? lv_color_hex(0xff6b6b) : lv_color_hex(0xcfd8dc),
                                0);
    lv_label_set_text(ctx->folder_status_label, msg);
}

/**
 * @brief Handle the folder creation action from the dialog.
 *
 * Triggered either by the "Create" button or keyboard READY event.
 * Validates the folder name, attempts to create it and updates status text
 * on error. On success, closes the dialog and reloads the browser view.
 *
 * @param e LVGL event with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_folder_create(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    const char *text = ctx->folder_textarea ? lv_textarea_get_text(ctx->folder_textarea) : NULL;
    if (!text) {
        file_browser_set_folder_status(ctx, "Invalid name", true);
        return;
    }

    char name[FS_NAV_MAX_NAME];
    strlcpy(name, text, sizeof(name));
    file_browser_trim_whitespace(name);
    if (!file_browser_is_valid_name(name)) {
        file_browser_set_folder_status(ctx, "Invalid folder name", true);
        return;
    }

    esp_err_t err = file_browser_create_folder(ctx, name);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            file_browser_set_folder_status(ctx,
                                           "Name already exists (FAT is case-insensitive)",
                                           true);
        } else {
            file_browser_set_folder_status(ctx, esp_err_to_name(err), true);
        }
        return;
    }

    file_browser_close_folder_dialog(ctx);
    esp_err_t reload = file_browser_reload();
    if (reload != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh after folder create: %s", esp_err_to_name(reload));
    }
}

/**
 * @brief Cancel handler for the "Create folder" dialog.
 *
 * Simply closes the dialog without creating a folder.
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_folder_cancel(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    file_browser_close_folder_dialog(ctx);
}

/**
 * @brief Set status text and color in the rename dialog.
 *
 * @param[in,out] ctx Browser context.
 * @param msg         Message text to display (must be non-NULL).
 * @param error       True to use error color, false for neutral/info color.
 */
static void file_browser_set_rename_status(file_browser_ctx_t *ctx, const char *msg, bool error)
{
    if (!ctx->rename_status_label || !msg) {
        return;
    }
    lv_obj_set_style_text_color(ctx->rename_status_label,
                                error ? lv_color_hex(0xff6b6b) : lv_color_hex(0xcfd8dc),
                                0);
    lv_label_set_text(ctx->rename_status_label, msg);
}

/**
 * @brief Show the rename dialog for the currently selected entry.
 *
 * Builds an overlay with a card containing the current entry name, a status label,
 * a text area prefilled with the existing name and a "Save"/"Cancel" button row,
 * plus an on-screen keyboard. Any existing rename dialog is closed first.
 *
 * @param[in,out] ctx Browser context with a valid @c action_entry.
 */
static void file_browser_show_rename_dialog(file_browser_ctx_t *ctx)
{
    if (!ctx || !ctx->action_entry.active) {
        return;
    }
    file_browser_close_rename_dialog(ctx);

    lv_obj_t *overlay = lv_obj_create(ctx->screen);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_center(overlay);
    ctx->rename_dialog = overlay;

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_gap(card, 10, 0);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Rename entry");

    ctx->rename_status_label = lv_label_create(card);
    lv_label_set_text(ctx->rename_status_label, "");

    ctx->rename_textarea = lv_textarea_create(card);
    lv_textarea_set_one_line(ctx->rename_textarea, true);
    lv_textarea_set_max_length(ctx->rename_textarea, FS_NAV_MAX_NAME - 1);
    lv_textarea_set_text(ctx->rename_textarea, ctx->action_entry.name);

    lv_obj_t *buttons = lv_obj_create(card);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(buttons, 8, 0);

    lv_obj_t *accept_btn = lv_button_create(buttons);
    lv_obj_t *accept_lbl = lv_label_create(accept_btn);
    lv_label_set_text(accept_lbl, "Save");
    lv_obj_set_style_text_align(accept_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(accept_btn, file_browser_on_rename_accept, LV_EVENT_CLICKED, ctx);

    lv_obj_t *cancel_btn = lv_button_create(buttons);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_align(cancel_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(cancel_btn, file_browser_on_rename_cancel, LV_EVENT_CLICKED, ctx);

    ctx->rename_keyboard = lv_keyboard_create(overlay);
    lv_keyboard_set_textarea(ctx->rename_keyboard, ctx->rename_textarea);
    lv_obj_add_state(ctx->rename_textarea, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ctx->rename_textarea, file_browser_on_rename_accept, LV_EVENT_READY, ctx);
}

/**
 * @brief Close and destroy the rename dialog overlay.
 *
 * Deletes the dialog overlay and clears all rename dialog-related pointers
 * in the context.
 *
 * @param[in,out] ctx Browser context that owns the dialog.
 */
static void file_browser_close_rename_dialog(file_browser_ctx_t *ctx)
{
    if (!ctx || !ctx->rename_dialog) {
        return;
    }
    lv_obj_del(ctx->rename_dialog);
    ctx->rename_dialog = NULL;
    ctx->rename_textarea = NULL;
    ctx->rename_status_label = NULL;
    ctx->rename_keyboard = NULL;
}

/**
 * @brief Create a folder in the current directory with the given name.
 *
 * Uses @c fs_nav_compose_path() to generate an absolute path and calls @c mkdir().
 *
 * @param[in,out] ctx Browser context providing the current path.
 * @param name        Folder name (already validated).
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if the folder already exists,
 *         ESP_FAIL on generic failure or errno-based errors.
 */
static esp_err_t file_browser_create_folder(file_browser_ctx_t *ctx, const char *name)
{
    char path[FS_NAV_MAX_PATH];
    esp_err_t err = fs_nav_compose_path(&ctx->nav, name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    if (mkdir(path, 0775) != 0) {
        if (errno == EEXIST) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGE(TAG, "mkdir(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Check if a given name is a valid filesystem entry name.
 *
 * Rejects empty strings and names containing '\', '/', ':', '*', '?', '"', '<', '>' or '|'.
 *
 * @param name Candidate name string.
 * @return true if the name is valid, false otherwise.
 */
static bool file_browser_is_valid_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (
                *p == '\\' || *p == '/' || *p == ':' ||
                *p == '*'  || *p == '?' || *p == '"' ||
                *p == '<'  || *p == '>' || *p == '|'
            ) 
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Trim leading and trailing whitespace characters from a string in-place.
 *
 * Whitespace considered: space, tab, newline and carriage return.
 *
 * @param[in,out] name String buffer to trim; may be shifted in memory.
 */
static void file_browser_trim_whitespace(char *name)
{
    if (!name) {
        return;
    }
    char *start = name;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    if (start != name) {
        memmove(name, start, (size_t)(end - start) + 1);
    }
}

/**
 * @brief Populate @c action_entry from a selected navigator entry.
 *
 * Copies flags, name and current directory into the context action entry
 * and marks it active.
 *
 * @param[in,out] ctx Browser context.
 * @param entry       Navigator entry to copy from.
 */
static void file_browser_prepare_action_entry(file_browser_ctx_t *ctx, const fs_nav_entry_t *entry)
{
    if (!ctx || !entry) {
        return;
    }
    ctx->action_entry.active = true;
    ctx->action_entry.is_dir = entry->is_dir;
    ctx->action_entry.is_txt = !entry->is_dir && fs_text_is_txt(entry->name);
    strlcpy(ctx->action_entry.name, entry->name, sizeof(ctx->action_entry.name));
    const char *dir = fs_nav_current_path(&ctx->nav);
    if (!dir) {
        dir = "";
    }
    strlcpy(ctx->action_entry.directory, dir, sizeof(ctx->action_entry.directory));
}

/**
 * @brief Close and clear the currently open action menu message box.
 *
 * If an action message box is present, closes it and nulls the pointer.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_close_action_menu(file_browser_ctx_t *ctx)
{
    if (ctx && ctx->action_mbox) {
        lv_msgbox_close(ctx->action_mbox);
        ctx->action_mbox = NULL;
    }
}

/**
 * @brief Show the action menu (Rename/Delete/Edit/Cancel) for current entry.
 *
 * Creates a message box containing the entry name and one or two button rows
 * depending on whether the entry is editable text or not.
 *
 * @param[in,out] ctx Browser context with an active @c action_entry.
 */
static void file_browser_show_action_menu(file_browser_ctx_t *ctx)
{
    if (!ctx || !ctx->action_entry.active) {
        return;
    }
    file_browser_close_action_menu(ctx);

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    ctx->action_mbox = mbox;
    lv_obj_set_style_max_width(mbox, LV_PCT(80), 0);
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text(label, ctx->action_entry.name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *footer = lv_obj_create(mbox);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(footer, 8, 0);

    lv_obj_t *row1 = lv_obj_create(footer);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row1, 8, 0);

    lv_obj_t *rename_btn = lv_button_create(row1);
    lv_obj_set_flex_grow(rename_btn, 1);
    lv_obj_t *rename_lbl = lv_label_create(rename_btn);
    lv_label_set_text(rename_lbl, "Rename");
    lv_obj_center(rename_lbl);
    lv_obj_set_user_data(rename_btn, (void *)FILE_BROWSER_ACTION_RENAME);
    lv_obj_add_event_cb(rename_btn, file_browser_on_action_button, LV_EVENT_CLICKED, ctx);

    lv_obj_t *del_btn = lv_button_create(row1);
    lv_obj_set_flex_grow(del_btn, 1);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, "Delete");
    lv_obj_center(del_lbl);
    lv_obj_set_user_data(del_btn, (void *)FILE_BROWSER_ACTION_DELETE);
    lv_obj_add_event_cb(del_btn, file_browser_on_action_button, LV_EVENT_CLICKED, ctx);

    bool has_edit = (!ctx->action_entry.is_dir && ctx->action_entry.is_txt);
    if (has_edit) {
        lv_obj_t *row2 = lv_obj_create(footer);
        lv_obj_remove_style_all(row2);
        lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(row2, 8, 0);

        lv_obj_t *edit_btn = lv_button_create(row2);
        lv_obj_set_flex_grow(edit_btn, 1);
        lv_obj_t *edit_lbl = lv_label_create(edit_btn);
        lv_label_set_text(edit_lbl, "Edit");
        lv_obj_center(edit_lbl);
        lv_obj_set_user_data(edit_btn, (void *)FILE_BROWSER_ACTION_EDIT);
        lv_obj_add_event_cb(edit_btn, file_browser_on_action_button, LV_EVENT_CLICKED, ctx);

        lv_obj_t *cancel_btn = lv_button_create(row2);
        lv_obj_set_flex_grow(cancel_btn, 1);
        lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, "Cancel");
        lv_obj_center(cancel_lbl);
        lv_obj_set_user_data(cancel_btn, (void *)FILE_BROWSER_ACTION_CANCEL);
        lv_obj_add_event_cb(cancel_btn, file_browser_on_action_button, LV_EVENT_CLICKED, ctx);
    } else {
        lv_obj_t *cancel_btn = lv_button_create(row1);
        lv_obj_set_flex_grow(cancel_btn, 1);
        lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, "Cancel");
        lv_obj_center(cancel_lbl);
        lv_obj_set_user_data(cancel_btn, (void *)FILE_BROWSER_ACTION_CANCEL);
        lv_obj_add_event_cb(cancel_btn, file_browser_on_action_button, LV_EVENT_CLICKED, ctx);
    }
}

/**
 * @brief Clear all transient action-related state from the context.
 *
 * Closes action and confirm dialogs, closes rename dialog and resets
 * the @c action_entry fields.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_clear_action_state(file_browser_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    file_browser_close_action_menu(ctx);
    file_browser_close_delete_confirm(ctx);
    file_browser_close_rename_dialog(ctx);
    ctx->action_entry.active = false;
    ctx->action_entry.is_dir = false;
    ctx->action_entry.is_txt = false;
    ctx->action_entry.name[0] = '\0';
    ctx->action_entry.directory[0] = '\0';
}

/**
 * @brief Handler for action menu buttons (Edit/Rename/Delete/Cancel).
 *
 * Reads the @c file_browser_action_type_t from button user data and performs
 * the corresponding action (open editor, show rename dialog, show delete
 * confirm, or cancel).
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_action_button(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    file_browser_action_type_t action = (file_browser_action_type_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    file_browser_close_action_menu(ctx);

    switch (action) {
        case FILE_BROWSER_ACTION_EDIT: {
            if (!ctx->action_entry.active || ctx->action_entry.is_dir || !ctx->action_entry.is_txt) {
                return;
            }
            char path[FS_NAV_MAX_PATH];
            if (file_browser_action_compose_path(ctx, path, sizeof(path)) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to compose path for edit");
                return;
            }
            text_viewer_open_opts_t opts = {
                .path = path,
                .return_screen = ctx->screen,
                .editable = true,
                .on_close = file_browser_editor_closed,
                .user_ctx = ctx,
            };
            esp_err_t err = text_viewer_open(&opts);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to edit \"%s\": %s", ctx->action_entry.name, esp_err_to_name(err));
            } else {
                file_browser_clear_action_state(ctx);
            }
            break;
        }
        case FILE_BROWSER_ACTION_RENAME:
            file_browser_show_rename_dialog(ctx);
            break;
        case FILE_BROWSER_ACTION_DELETE:
            file_browser_show_delete_confirm(ctx);
            break;
        case FILE_BROWSER_ACTION_CANCEL:
        default:
            file_browser_clear_action_state(ctx);
            break;
    }
}

/**
 * @brief Close and clear the delete confirmation message box.
 *
 * If a confirmation message box is present, closes it and nulls the pointer.
 *
 * @param[in,out] ctx Browser context.
 */
static void file_browser_close_delete_confirm(file_browser_ctx_t *ctx)
{
    if (ctx && ctx->confirm_mbox) {
        lv_msgbox_close(ctx->confirm_mbox);
        ctx->confirm_mbox = NULL;
    }
}

/**
 * @brief Show a Yes/No confirmation dialog for deleting the selected entry.
 *
 * Creates a message box with the entry name in the prompt and two footer
 * buttons: "Yes" and "No".
 *
 * @param[in,out] ctx Browser context with an active @c action_entry.
 */
static void file_browser_show_delete_confirm(file_browser_ctx_t *ctx)
{
    if (!ctx || !ctx->action_entry.active) {
        return;
    }
    file_browser_close_delete_confirm(ctx);

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    ctx->confirm_mbox = mbox;
    lv_obj_set_style_max_width(mbox, LV_PCT(80), 0);
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text_fmt(label, "Delete \"%s\"?", ctx->action_entry.name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *yes_btn = lv_msgbox_add_footer_button(mbox, "Yes");
    lv_obj_set_user_data(yes_btn, (void *)1);
    lv_obj_add_event_cb(yes_btn, file_browser_on_delete_confirm, LV_EVENT_CLICKED, ctx);

    lv_obj_t *no_btn = lv_msgbox_add_footer_button(mbox, "No");
    lv_obj_set_user_data(no_btn, (void *)0);
    lv_obj_add_event_cb(no_btn, file_browser_on_delete_confirm, LV_EVENT_CLICKED, ctx);
}

/**
 * @brief Handler for delete confirmation buttons ("Yes"/"No").
 *
 * If confirmed, attempts to delete the selected entry. Otherwise, clears
 * the action state.
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_delete_confirm(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    bool confirm = (bool)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    file_browser_close_delete_confirm(ctx);

    if (!confirm) {
        file_browser_clear_action_state(ctx);
        return;
    }

    esp_err_t err = file_browser_delete_selected_entry(ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Delete failed: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Delete the currently selected action entry and reload the browser.
 *
 * Composes the full path, recursively deletes the target (if directory) and
 * reloads the file browser view on success.
 *
 * @param[in,out] ctx Browser context with an active @c action_entry.
 * @return ESP_OK on success or appropriate error code.
 */
static esp_err_t file_browser_delete_selected_entry(file_browser_ctx_t *ctx)
{
    if (!ctx || !ctx->action_entry.active) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[FS_NAV_MAX_PATH];
    esp_err_t err = file_browser_action_compose_path(ctx, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    err = file_browser_delete_path(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete %s: %s", path, esp_err_to_name(err));
        return err;
    }

    file_browser_clear_action_state(ctx);
    return file_browser_reload();
}

/**
 * @brief Compose a full filesystem path from @c action_entry.directory and name.
 *
 * Assembles "<directory>/<name>" into the provided output buffer.
 *
 * @param ctx      Browser context with an active @c action_entry.
 * @param[out] out Output buffer for the composed path.
 * @param out_len  Size of @p out buffer in bytes.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_STATE if state is invalid,
 *         ESP_ERR_INVALID_SIZE if the buffer is too small.
 */
static esp_err_t file_browser_action_compose_path(const file_browser_ctx_t *ctx, char *out, size_t out_len)
{
    if (!ctx || !ctx->action_entry.active || !out || out_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->action_entry.directory[0] == '\0' || ctx->action_entry.name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int needed = snprintf(out, out_len, "%s/%s", ctx->action_entry.directory, ctx->action_entry.name);
    if (needed < 0 || needed >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief Accept handler for the rename dialog (button or keyboard).
 *
 * Validates the new name, checks for no-op, attempts rename via
 * @c file_browser_perform_rename(), displays any errors in the dialog and,
 * on success, closes the dialog and reloads the browser.
 *
 * @param e LVGL event (LV_EVENT_CLICKED or LV_EVENT_READY) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_rename_accept(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->rename_textarea) {
        return;
    }

    const char *text = lv_textarea_get_text(ctx->rename_textarea);
    if (!text) {
        file_browser_set_rename_status(ctx, "Invalid name", true);
        return;
    }

    char name[FS_NAV_MAX_NAME];
    strlcpy(name, text, sizeof(name));
    file_browser_trim_whitespace(name);
    if (!file_browser_is_valid_name(name)) {
        file_browser_set_rename_status(ctx, "Invalid name", true);
        return;
    }

    if (strcmp(name, ctx->action_entry.name) == 0) {
        file_browser_close_rename_dialog(ctx);
        file_browser_clear_action_state(ctx);
        return;
    }

    esp_err_t err = file_browser_perform_rename(ctx, name);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            file_browser_set_rename_status(ctx,
                                           "Name already exists (FAT is case-insensitive)",
                                           true);
        } else {
            file_browser_set_rename_status(ctx, esp_err_to_name(err), true);
        }
        return;
    }

    file_browser_close_rename_dialog(ctx);
    file_browser_clear_action_state(ctx);
    esp_err_t reload = file_browser_reload();
    if (reload != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh after rename: %s", esp_err_to_name(reload));
    }
}

/**
 * @brief Cancel handler for the rename dialog.
 *
 * Closes the dialog and clears action state without renaming.
 *
 * @param e LVGL event (LV_EVENT_CLICKED) with user data = @c file_browser_ctx_t*.
 */
static void file_browser_on_rename_cancel(lv_event_t *e)
{
    file_browser_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    file_browser_close_rename_dialog(ctx);
    file_browser_clear_action_state(ctx);
}

/**
 * @brief Perform the actual filesystem rename for the current action entry.
 *
 * Builds the old and new paths and calls @c rename(). If the destination
 * already exists, returns ESP_ERR_INVALID_STATE.
 *
 * @param[in,out] ctx   Browser context with an active @c action_entry.
 * @param new_name      New entry name (validated, non-empty).
 * @return ESP_OK on success or an appropriate ESP_ERR_* code on failure.
 */
static esp_err_t file_browser_perform_rename(file_browser_ctx_t *ctx, const char *new_name)
{
    if (!ctx || !ctx->action_entry.active || !new_name || new_name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char old_path[FS_NAV_MAX_PATH];
    esp_err_t err = file_browser_action_compose_path(ctx, old_path, sizeof(old_path));
    if (err != ESP_OK) {
        return err;
    }

    char new_path[FS_NAV_MAX_PATH];
    int needed = snprintf(new_path, sizeof(new_path), "%s/%s", ctx->action_entry.directory, new_name);
    if (needed < 0 || needed >= (int)sizeof(new_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (rename(old_path, new_path) != 0) {
        if (errno == EEXIST) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGE(TAG, "rename(%s -> %s) failed (errno=%d)", old_path, new_path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Recursively delete a path, which may be a file or directory tree.
 *
 * Uses @c stat() to determine whether the path is a directory. If so, iterates
 * over entries with @c opendir()/readdir(), recursively deletes children and
 * finally removes the directory. If it is a file, calls @c remove().
 *
 * Missing paths (ENOENT) are treated as success.
 *
 * @param path Path to delete (must be non-empty string).
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_ARG for invalid input,
 *         ESP_ERR_INVALID_SIZE if child path buffer would overflow,
 *         ESP_FAIL on other filesystem/errno-based errors.
 */
static esp_err_t file_browser_delete_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "stat(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            ESP_LOGE(TAG, "opendir(%s) failed (errno=%d)", path, errno);
            return ESP_FAIL;
        }
        struct dirent *dent = NULL;
        while ((dent = readdir(dir)) != NULL) {
            if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
                continue;
            }
            char child[FS_NAV_MAX_PATH];
            int needed = snprintf(child, sizeof(child), "%s/%s", path, dent->d_name);
            if (needed < 0 || needed >= (int)sizeof(child)) {
                closedir(dir);
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t err = file_browser_delete_path(child);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
        }
        closedir(dir);
        if (rmdir(path) != 0) {
            ESP_LOGE(TAG, "rmdir(%s) failed (errno=%d)", path, errno);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "remove(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}
