#include "text_viewer_screen.h"

#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "fs_navigator.h"
#include "fs_text_ops.h"
#include "esp_log.h"
#include "sd_card.h"

/**
 * @brief Runtime state for the singleton text viewer/editor screen.
 */
typedef struct {
    bool active;                 /**< True while the viewer screen is active */
    bool editable;               /**< True if edit mode is enabled */
    bool dirty;                  /**< True if current text differs from original */
    bool suppress_events;        /**< Temporarily disable change detection */
    bool new_file;               /**< True if creating a new file */
    lv_obj_t *screen;            /**< Root LVGL screen object */
    lv_obj_t *toolbar;           /**< Toolbar container */
    lv_obj_t *path_label;        /**< Label showing the file path */
    lv_obj_t *status_label;      /**< Label showing transient status messages */
    lv_obj_t *save_btn;          /**< Save button (hidden/disabled in view mode) */
    lv_obj_t *text_area;         /**< Text area for viewing/editing content */
    lv_obj_t *keyboard;          /**< On-screen keyboard */
    lv_obj_t *return_screen;     /**< Screen to return to on close */
    lv_obj_t *confirm_mbox;      /**< Confirmation message box (save/discard) */
    lv_obj_t *name_dialog;       /**< Filename prompt dialog */
    lv_obj_t *name_textarea;     /**< Text area used inside filename dialog */
    text_viewer_close_cb_t close_cb;  /**< Optional close callback */
    void *close_ctx;             /**< User context for close callback */
    char path[FS_TEXT_MAX_PATH]; /**< Current file path */
    char directory[FS_TEXT_MAX_PATH]; /**< Directory used for new files */
    char pending_name[FS_NAV_MAX_NAME]; /**< Suggested filename for new files */
    char *original_text;         /**< Snapshot of text at load/save time */
} text_viewer_ctx_t;

/**
 * @brief Confirmation actions used in the save/discard dialog.
 */
typedef enum {
    TEXT_VIEWER_CONFIRM_SAVE = 1,    /**< Confirm saving changes */
    TEXT_VIEWER_CONFIRM_DISCARD = 2, /**< Confirm discarding changes */
} text_viewer_confirm_action_t;

static const char *TAG = "text_viewer";

static text_viewer_ctx_t s_viewer;

/************************************** UI Setup & State *************************************/

/**
 * @brief Build all LVGL widgets for the viewer/editor screen.
 *
 * Creates toolbar, labels, text area, and on-screen keyboard.
 * Does not load content or set mode; see @ref text_viewer_open and
 * @ref text_viewer_apply_mode.
 *
 * @param ctx Viewer context (must be non-NULL).
 */
static void text_viewer_build_screen(text_viewer_ctx_t *ctx);

/**
 * @brief Apply current mode (view vs edit) to widgets and controls.
 *
 * Enables/disables text area, toggles keyboard and save button,
 * then updates button states.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_apply_mode(text_viewer_ctx_t *ctx);

/**
 * @brief Set a short status message in the toolbar.
 *
 * @param ctx Viewer context.
 * @param msg Null-terminated message string (ignored if NULL).
 */
static void text_viewer_set_status(text_viewer_ctx_t *ctx, const char *msg);

/**
 * @brief Replace the stored original text snapshot.
 *
 * Frees the previous snapshot and stores a duplicate of @p text.
 *
 * @param ctx  Viewer context.
 * @param text New baseline text (may be NULL, which clears the snapshot).
 */
static void text_viewer_set_original(text_viewer_ctx_t *ctx, const char *text);

/**
 * @brief Enable/disable the Save button based on @c editable and @c dirty.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_update_buttons(text_viewer_ctx_t *ctx);

/*********************************************************************************************/


/******************************* Keyboard & interaction helpers ******************************/

/**
 * @brief Explicitly show the keyboard in edit mode.
 *
 * @param ctx    Viewer context.
 * @param target Text area to focus (falls back to main text area if NULL).
 */
static void text_viewer_show_keyboard(text_viewer_ctx_t *ctx, lv_obj_t *target);

/**
 * @brief Explicitly hide the keyboard and detach it from the textarea.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_hide_keyboard(text_viewer_ctx_t *ctx);

/**
 * @brief Show the keyboard when the text area is tapped in edit mode.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_text_area_clicked(lv_event_t *e);

/**
 * @brief Hide the keyboard when its cancel/close button is pressed.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_keyboard_cancel(lv_event_t *e);

/**
 * @brief Show the keyboard when the filename dialog textarea is tapped.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_name_textarea_clicked(lv_event_t *e);

/**
 * @brief Trigger Save when the keyboard's OK button is pressed.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_keyboard_ready(lv_event_t *e);

/**
 * @brief Hide the keyboard when tapping outside editable widgets.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_screen_clicked(lv_event_t *e);

/*********************************************************************************************/


/************************************** Editing workflow *************************************/

/**
 * @brief Text change handler: updates @c dirty, Save button, and status.
 *
 * Ignored when not editable or when @c suppress_events is true.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_text_changed(lv_event_t *e);

/**
 * @brief Save handler: writes current text to file and updates status.
 *
 * Logs and shows an error status if the write fails.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_handle_save(text_viewer_ctx_t *ctx);

/**
 * @brief "Save" button event handler.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_save(lv_event_t *e);

/**
 * @brief "Back" button handler: closes the screen or prompts to save/discard.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_back(lv_event_t *e);

/*********************************************************************************************/

/**
 * @brief Validate candidate filename (must end with .txt and contain safe chars).
 */
static bool text_viewer_validate_name(const char *name);

/**
 * @brief Ensure \".txt\" suffix (adds or fixes trailing dot cases).
 */
static void text_viewer_ensure_txt_extension(char *name, size_t len);

/**
 * @brief Compose absolute path for a new file.
 *
 * @param ctx     Viewer context (requires valid directory).
 * @param name    Filename to append.
 * @param out     Destination buffer for resulting path.
 * @param out_len Size of @p out in bytes.
 * @return ESP_OK on success or ESP_ERR_* on failure.
 */
static esp_err_t text_viewer_compose_new_path(text_viewer_ctx_t *ctx, const char *name, char *out, size_t out_len);

/**
 * @brief Check if a filesystem path already exists.
 *
 * @param path Absolute path to test.
 * @return true if the path exists, false otherwise.
 */
static bool text_viewer_path_exists(const char *path);

/**
 * @brief Show the filename dialog used when saving a new file.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_show_name_dialog(text_viewer_ctx_t *ctx);

/**
 * @brief Close the filename dialog (if present) and restore edit state.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_close_name_dialog(text_viewer_ctx_t *ctx);

/**
 * @brief Filename dialog button handler.
 *
 * @param e LVGL event.
 */
static void text_viewer_on_name_dialog(lv_event_t *e);

/*********************************************************************************************/


/************************************ Confirmation dialog ************************************/

/**
 * @brief Show the save/discard/cancel confirmation dialog.
 *
 * No-op if a dialog is already open.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_show_confirm(text_viewer_ctx_t *ctx);

/**
 * @brief Check if a target object is inside (or is) a given parent object.
 *
 * @param parent  The LVGL object considered as the potential ancestor.
 * @param target  The LVGL object whose ancestry is checked.
 */
static bool text_viewer_target_in(lv_obj_t *parent, lv_obj_t *target);

/**
 * @brief Close and clear the confirmation dialog if present.
 *
 * @param ctx Viewer context.
 */
static void text_viewer_close_confirm(text_viewer_ctx_t *ctx);

/**
 * @brief Confirmation dialog button handler (Save / Discard / Cancel).
 *
 * @param e LVGL event.
 */
static void text_viewer_on_confirm(lv_event_t *e);

/**
 * @brief Close the viewer, unload the screen, and invoke the close callback.
 *
 * Resets mode and frees resources (keyboard target, original snapshot).
 *
 * @param ctx     Viewer context.
 * @param changed True if file content was saved/changed (passed to callback).
 */
static void text_viewer_close(text_viewer_ctx_t *ctx, bool changed);

/*********************************************************************************************/

esp_err_t text_viewer_open(const text_viewer_open_opts_t *opts)
{
    if (!opts || !opts->return_screen) {
        return ESP_ERR_INVALID_ARG;
    }

    bool new_file = !opts->path || opts->path[0] == '\0';
    if (!new_file && !opts->path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (new_file && (!opts->directory || opts->directory[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    char *content = NULL;
    if (new_file) {
        content = strdup("");
        if (!content) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        size_t len = 0;
        esp_err_t err = fs_text_read_range(opts->path, 0, &content, &len);
        if (err != ESP_OK) {
            return err;
        }
    }

    text_viewer_ctx_t *ctx = &s_viewer;
    if (!ctx->screen) {
        text_viewer_build_screen(ctx);
    }

    text_viewer_close_confirm(ctx);
    ctx->active = true;
    ctx->editable = new_file ? true : opts->editable;
    ctx->new_file = new_file;
    ctx->dirty = new_file ? true : false;
    ctx->suppress_events = true;
    ctx->return_screen = opts->return_screen;
    ctx->close_cb = opts->on_close;
    ctx->close_ctx = opts->user_ctx;

    ctx->name_dialog = NULL;
    ctx->name_textarea = NULL;

    if (new_file) {
        ctx->path[0] = '\0';
        strlcpy(ctx->directory, opts->directory, sizeof(ctx->directory));
        strlcpy(ctx->pending_name, ".txt", sizeof(ctx->pending_name));
        lv_label_set_text(ctx->path_label, "");
    } else {
        ctx->directory[0] = '\0';
        ctx->pending_name[0] = '\0';
        strlcpy(ctx->path, opts->path, sizeof(ctx->path));
        lv_label_set_text(ctx->path_label, ctx->path);
    }

    lv_textarea_set_text(ctx->text_area, content);
    text_viewer_set_original(ctx, content);
    free(content);
    ctx->suppress_events = false;
    if (ctx->new_file) {
        text_viewer_set_status(ctx, "New TXT");
    } else {
        text_viewer_set_status(ctx, ctx->editable ? "Edit mode" : "View mode");
    }
    text_viewer_apply_mode(ctx);
    lv_screen_load(ctx->screen);
    if (ctx->new_file) {
        lv_textarea_set_cursor_pos(ctx->text_area, 0);
        lv_obj_add_state(ctx->text_area, LV_STATE_FOCUSED);
        text_viewer_show_keyboard(ctx, ctx->text_area);
    }
    return ESP_OK;
}

static void text_viewer_build_screen(text_viewer_ctx_t *ctx)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_gap(scr, 6, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, text_viewer_on_screen_clicked, LV_EVENT_CLICKED, ctx);
    ctx->screen = scr;

    lv_obj_t *toolbar = lv_obj_create(scr);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(toolbar, 8, 0);
    ctx->toolbar = toolbar;

    lv_obj_t *back_btn = lv_button_create(toolbar);
    lv_obj_add_event_cb(back_btn, text_viewer_on_back, LV_EVENT_CLICKED, ctx);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    ctx->save_btn = lv_button_create(toolbar);
    lv_obj_add_event_cb(ctx->save_btn, text_viewer_on_save, LV_EVENT_CLICKED, ctx);
    lv_obj_t *save_lbl = lv_label_create(ctx->save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(save_lbl);

    ctx->status_label = lv_label_create(toolbar);
    lv_label_set_text(ctx->status_label, "");
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(ctx->status_label, 1);
    const lv_font_t *status_font = lv_obj_get_style_text_font(ctx->status_label, LV_PART_MAIN);
    lv_coord_t status_height = status_font ? status_font->line_height : 18;
    lv_obj_set_style_min_height(ctx->status_label, status_height, 0);
    lv_obj_set_style_max_height(ctx->status_label, status_height, 0);

    ctx->path_label = lv_label_create(scr);
    lv_label_set_long_mode(ctx->path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(ctx->path_label, "");

    ctx->text_area = lv_textarea_create(scr);
    lv_obj_set_flex_grow(ctx->text_area, 1);
    lv_textarea_set_cursor_click_pos(ctx->text_area, false);
    lv_obj_set_scrollbar_mode(ctx->text_area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_width(ctx->text_area, LV_PCT(100));
    lv_obj_add_event_cb(ctx->text_area, text_viewer_on_text_changed, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(ctx->text_area, text_viewer_on_text_area_clicked, LV_EVENT_CLICKED, ctx);

    ctx->keyboard = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(ctx->keyboard, ctx->text_area);
    lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ctx->keyboard, text_viewer_on_keyboard_cancel, LV_EVENT_CANCEL, ctx);
    lv_obj_add_event_cb(ctx->keyboard, text_viewer_on_keyboard_ready, LV_EVENT_READY, ctx);
}

static void text_viewer_apply_mode(text_viewer_ctx_t *ctx)
{
    if (ctx->editable) {
        lv_obj_clear_state(ctx->text_area, LV_STATE_DISABLED);
        lv_textarea_set_cursor_click_pos(ctx->text_area, true);
        lv_obj_add_flag(ctx->text_area, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        text_viewer_hide_keyboard(ctx);
        lv_obj_clear_flag(ctx->save_btn, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_cursor_pos(ctx->text_area, 0);
    } else {
        lv_textarea_set_cursor_click_pos(ctx->text_area, false);
        lv_obj_clear_flag(ctx->text_area, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        text_viewer_hide_keyboard(ctx);
        lv_obj_add_flag(ctx->save_btn, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_clear_selection(ctx->text_area);
    }
    lv_obj_scroll_to_y(ctx->text_area, 0, LV_ANIM_OFF);
    text_viewer_update_buttons(ctx);
}

static void text_viewer_set_status(text_viewer_ctx_t *ctx, const char *msg)
{
    if (ctx->status_label && msg) {
        lv_label_set_text(ctx->status_label, msg);
    }
}

static void text_viewer_set_original(text_viewer_ctx_t *ctx, const char *text)
{
    free(ctx->original_text);
    ctx->original_text = text ? strdup(text) : NULL;
}

static void text_viewer_update_buttons(text_viewer_ctx_t *ctx)
{
    if (!ctx->editable) {
        return;
    }
    if (ctx->dirty) {
        lv_obj_clear_state(ctx->save_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(ctx->save_btn, LV_STATE_DISABLED);
    }
}

static void text_viewer_show_keyboard(text_viewer_ctx_t *ctx, lv_obj_t *target)
{
    if (!ctx || !ctx->editable) {
        return;
    }
    if (target) {
        lv_keyboard_set_textarea(ctx->keyboard, target);
    } else if (!lv_keyboard_get_textarea(ctx->keyboard)) {
        lv_keyboard_set_textarea(ctx->keyboard, ctx->text_area);
    }
    lv_obj_clear_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void text_viewer_hide_keyboard(text_viewer_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (!lv_obj_has_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_keyboard_get_textarea(ctx->keyboard)) {
        lv_keyboard_set_textarea(ctx->keyboard, NULL);
    }
}

static void text_viewer_on_text_area_clicked(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->editable) {
        return;
    }
    text_viewer_show_keyboard(ctx, ctx->text_area);
}

static void text_viewer_on_keyboard_cancel(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    text_viewer_hide_keyboard(ctx);
}

static void text_viewer_on_name_textarea_clicked(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->name_textarea) {
        return;
    }
    text_viewer_show_keyboard(ctx, ctx->name_textarea);
}

static void text_viewer_on_keyboard_ready(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->editable) {
        return;
    }
    text_viewer_handle_save(ctx);
}

static void text_viewer_on_screen_clicked(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->editable) {
        return;
    }
    if (ctx->name_dialog) {
        return;
    }
    if (lv_obj_has_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    if (text_viewer_target_in(ctx->text_area, target) ||
        text_viewer_target_in(ctx->keyboard, target)) {
        return;
    }
    text_viewer_hide_keyboard(ctx);
}

static void text_viewer_on_text_changed(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->editable || ctx->suppress_events) {
        return;
    }
    const char *text = lv_textarea_get_text(ctx->text_area);
    const char *orig = ctx->original_text ? ctx->original_text : "";
    bool dirty = strcmp(text, orig) != 0;
    if (dirty != ctx->dirty) {
        ctx->dirty = dirty;
        text_viewer_update_buttons(ctx);
        text_viewer_set_status(ctx, dirty ? "Modified" : "Saved");
    }
}

static void text_viewer_handle_save(text_viewer_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->new_file && ctx->path[0] == '\0') {
        text_viewer_show_name_dialog(ctx);
        return;
    }

    const char *text = lv_textarea_get_text(ctx->text_area);
    if (!text) {
        text = "";
    }

    if (ctx->path[0] == '\0') {
        text_viewer_set_status(ctx, "Missing file name");
        return;
    }

    const char *dest_path = ctx->path;
    esp_err_t err = fs_text_write(dest_path, text, strlen(text));
    if (err != ESP_OK) {
        text_viewer_set_status(ctx, esp_err_to_name(err));
        ESP_LOGE(TAG, "Failed to save %s: %s", dest_path, esp_err_to_name(err));
        sdspi_schedule_sd_retry();
        return;
    }

    text_viewer_set_original(ctx, text);
    ctx->dirty = false;
    text_viewer_set_status(ctx, "Saved");
}

static void text_viewer_on_save(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    text_viewer_handle_save(ctx);
}

static void text_viewer_on_back(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    if (ctx->editable && ctx->dirty) {
        text_viewer_show_confirm(ctx);
        return;
    }
    text_viewer_close(ctx, false);
}

/************************************* New-file utilities *************************************/

static bool text_viewer_validate_name(const char *name)
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
    return fs_text_is_txt(name);
}

static void text_viewer_ensure_txt_extension(char *name, size_t len)
{
    if (!name || len == 0) {
        return;
    }
    size_t n = strlen(name);
    if (n == 0) {
        strlcpy(name, ".txt", len);
        return;
    }
    const char *dot = strrchr(name, '.');
    if (dot && dot[1] != '\0') {
        if (strcasecmp(dot, ".txt") == 0) {
            return;
        }
        return;
    }
    if (n + 4 >= len) {
        return;
    }
    if (dot && dot[1] == '\0') {
        strlcpy(name + n, "txt", len - n);
    } else {
        strlcat(name, ".txt", len);
    }
}

static esp_err_t text_viewer_compose_new_path(text_viewer_ctx_t *ctx, const char *name, char *out, size_t out_len)
{
    if (!ctx || !name || !out || out_len == 0 || ctx->directory[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int needed = snprintf(out, out_len, "%s/%s", ctx->directory, name);
    if (needed < 0 || needed >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool text_viewer_path_exists(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }
    struct stat st = {0};
    return stat(path, &st) == 0;
}

static void text_viewer_show_name_dialog(text_viewer_ctx_t *ctx)
{
    if (!ctx || !ctx->new_file || !ctx->editable || ctx->name_dialog) {
        return;
    }
    lv_obj_t *dlg = lv_msgbox_create(ctx->screen);
    ctx->name_dialog = dlg;
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_max_width(dlg, LV_PCT(65), 0);
    lv_obj_set_width(dlg, LV_PCT(65));

    lv_obj_t *content = lv_msgbox_get_content(dlg);
    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, "File name");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

    ctx->name_textarea = lv_textarea_create(content);
    lv_textarea_set_one_line(ctx->name_textarea, true);
    lv_textarea_set_max_length(ctx->name_textarea, FS_NAV_MAX_NAME - 1);
    const char *initial = ctx->pending_name[0] ? ctx->pending_name : ".txt";
    lv_textarea_set_text(ctx->name_textarea, initial);
    lv_textarea_set_cursor_pos(ctx->name_textarea, 0);
    lv_obj_add_state(ctx->name_textarea, LV_STATE_FOCUSED);
    lv_obj_clear_state(ctx->text_area, LV_STATE_FOCUSED);
    lv_obj_add_state(ctx->text_area, LV_STATE_DISABLED);
    lv_textarea_set_cursor_click_pos(ctx->text_area, false);

    lv_obj_t *save_btn = lv_msgbox_add_footer_button(dlg, "Save");
    lv_obj_set_user_data(save_btn, (void *)1);
    lv_obj_add_event_cb(save_btn, text_viewer_on_name_dialog, LV_EVENT_CLICKED, ctx);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(dlg, "Cancel");
    lv_obj_set_user_data(cancel_btn, (void *)0);
    lv_obj_add_event_cb(cancel_btn, text_viewer_on_name_dialog, LV_EVENT_CLICKED, ctx);

    text_viewer_show_keyboard(ctx, ctx->name_textarea);
    lv_obj_add_event_cb(ctx->name_textarea, text_viewer_on_name_textarea_clicked, LV_EVENT_CLICKED, ctx);

    lv_obj_update_layout(ctx->keyboard);
    lv_obj_update_layout(dlg);
    lv_coord_t keyboard_top = lv_obj_get_y(ctx->keyboard);
    lv_coord_t dialog_h = lv_obj_get_height(dlg);
    lv_coord_t margin = 10;
    if (keyboard_top > dialog_h) {
        lv_coord_t candidate = (keyboard_top - dialog_h) / 2;
        if (candidate > 0) {
            margin = candidate;
        }
    }
    lv_obj_align(dlg, LV_ALIGN_TOP_MID, 0, margin);
}

static void text_viewer_close_name_dialog(text_viewer_ctx_t *ctx)
{
    if (!ctx || !ctx->name_dialog) {
        return;
    }
    if (ctx->name_textarea) {
        const char *current = lv_textarea_get_text(ctx->name_textarea);
        if (current) {
            strlcpy(ctx->pending_name, current, sizeof(ctx->pending_name));
        }
    }
    lv_msgbox_close(ctx->name_dialog);
    ctx->name_dialog = NULL;
    ctx->name_textarea = NULL;
    lv_obj_clear_state(ctx->text_area, LV_STATE_DISABLED);
    lv_textarea_set_cursor_click_pos(ctx->text_area, true);
    text_viewer_hide_keyboard(ctx);
}

static void text_viewer_on_name_dialog(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->name_dialog) {
        return;
    }
    bool confirm = (bool)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (!confirm) {
        text_viewer_close_name_dialog(ctx);
        return;
    }

    const char *raw = ctx->name_textarea ? lv_textarea_get_text(ctx->name_textarea) : "";
    char name_buf[FS_NAV_MAX_NAME];
    strlcpy(name_buf, raw ? raw : "", sizeof(name_buf));
    text_viewer_ensure_txt_extension(name_buf, sizeof(name_buf));
    if (!text_viewer_validate_name(name_buf)) {
        text_viewer_set_status(ctx, "Invalid .txt name");
        return;
    }
    esp_err_t compose_err = text_viewer_compose_new_path(ctx, name_buf, ctx->path, sizeof(ctx->path));
    if (compose_err != ESP_OK) {
        text_viewer_set_status(ctx, "Path too long");
        return;
    }
    if (text_viewer_path_exists(ctx->path)) {
        text_viewer_set_status(ctx, "File already exists");
        return;
    }

    strlcpy(ctx->pending_name, name_buf, sizeof(ctx->pending_name));
    ctx->directory[0] = '\0';
    ctx->new_file = false;
    lv_label_set_text(ctx->path_label, ctx->path);
    text_viewer_close_name_dialog(ctx);
    text_viewer_handle_save(ctx);
}

static void text_viewer_show_confirm(text_viewer_ctx_t *ctx)
{
    if (ctx->confirm_mbox) {
        return;
    }
    lv_obj_t *mbox = lv_msgbox_create(ctx->screen);
    lv_obj_add_flag(mbox, LV_OBJ_FLAG_FLOATING);
    ctx->confirm_mbox = mbox;
    lv_obj_set_style_max_width(mbox, LV_PCT(80), 0);
    lv_obj_set_width(mbox, LV_PCT(80));
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text(label, "Save changes?");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *save_btn = lv_msgbox_add_footer_button(mbox, "Save");
    lv_obj_set_user_data(save_btn, (void *)TEXT_VIEWER_CONFIRM_SAVE);
    lv_obj_set_flex_grow(save_btn, 1);
    lv_obj_add_event_cb(save_btn, text_viewer_on_confirm, LV_EVENT_CLICKED, ctx);

    lv_obj_t *discard_btn = lv_msgbox_add_footer_button(mbox, "Discard");
    lv_obj_set_user_data(discard_btn, (void *)TEXT_VIEWER_CONFIRM_DISCARD);
    lv_obj_set_flex_grow(discard_btn, 1);
    lv_obj_add_event_cb(discard_btn, text_viewer_on_confirm, LV_EVENT_CLICKED, ctx);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_set_user_data(cancel_btn, NULL);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_add_event_cb(cancel_btn, text_viewer_on_confirm, LV_EVENT_CLICKED, ctx);
}

static bool text_viewer_target_in(lv_obj_t *parent, lv_obj_t *target)
{
    while (target) {
        if (target == parent) {
            return true;
        }
        target = lv_obj_get_parent(target);
    }
    return false;
}

static void text_viewer_close_confirm(text_viewer_ctx_t *ctx)
{
    if (ctx->confirm_mbox) {
        lv_msgbox_close(ctx->confirm_mbox);
        ctx->confirm_mbox = NULL;
    }
}

static void text_viewer_on_confirm(lv_event_t *e)
{
    text_viewer_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    void *ud = lv_obj_get_user_data(lv_event_get_target(e));
    text_viewer_close_confirm(ctx);
    if (ud == (void *)TEXT_VIEWER_CONFIRM_SAVE) {
        text_viewer_handle_save(ctx);
    } else if (ud == (void *)TEXT_VIEWER_CONFIRM_DISCARD) {
        text_viewer_close(ctx, false);
    }
}

static void text_viewer_close(text_viewer_ctx_t *ctx, bool changed)
{
    text_viewer_close_confirm(ctx);
    text_viewer_close_name_dialog(ctx);
    ctx->active = false;
    ctx->editable = false;
    ctx->dirty = false;
    ctx->suppress_events = false;
    ctx->new_file = false;
    ctx->directory[0] = '\0';
    ctx->pending_name[0] = '\0';
    lv_keyboard_set_textarea(ctx->keyboard, NULL);
    lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    free(ctx->original_text);
    ctx->original_text = NULL;
    if (ctx->return_screen) {
        lv_screen_load(ctx->return_screen);
    }
    if (ctx->close_cb) {
        ctx->close_cb(changed, ctx->close_ctx);
    }
}

