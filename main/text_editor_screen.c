#include "text_editor_screen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "fs_navigator.h"
#include "fs_text_ops.h"

/**
 * @brief Internal confirmation dialog modes used by the text editor.
 */
typedef enum {
    TEXT_EDITOR_CONFIRM_NONE = 0,   /**< No confirmation pending */
    TEXT_EDITOR_CONFIRM_DISCARD,    /**< Confirm discarding unsaved changes */
    TEXT_EDITOR_CONFIRM_DELETE,     /**< Confirm deleting a file */
} text_editor_confirm_t;

/**
 * @brief Internal context structure for the text editor screen.
 */
typedef struct {
    bool active;                        /**< True if editor is currently open */
    bool dirty;                         /**< True if file has unsaved changes */
    bool fs_changed;                    /**< True if filesystem content changed */
    bool suppress_events;               /**< Temporarily disables event handling */
    lv_obj_t *screen;                   /**< Main LVGL screen object */
    lv_obj_t *toolbar;                  /**< Toolbar container */
    lv_obj_t *status_label;             /**< Status message label */
    lv_obj_t *name_input;               /**< Textarea for file name */
    lv_obj_t *body_input;               /**< Textarea for file content */
    lv_obj_t *keyboard;                 /**< On-screen keyboard object */
    lv_obj_t *confirm_dialog;           /**< Confirmation dialog box */
    lv_obj_t *return_screen;            /**< Screen to return to after closing */
    text_editor_close_cb_t close_cb;    /**< Optional close callback */
    void *close_ctx;                    /**< User context for close callback */
    text_editor_confirm_t confirm_mode; /**< Current confirmation mode */
    char directory[FS_TEXT_MAX_PATH];   /**< Directory path buffer */
    char path[FS_TEXT_MAX_PATH];        /**< Full file path buffer */
} text_editor_ctx_t;

static text_editor_ctx_t s_editor;

/**
 * @brief Build and initialize all LVGL UI elements for the text editor.
 *
 * Creates the editor screen with a toolbar (Back/Save/Delete + status label),
 * a filename field, the main text area, and an on-screen keyboard. This
 * function only constructs widgets and wires their event handlers; it does not
 * load any file content or set focus/visibility (handled by @ref text_editor_open).
 *
 * @param ctx Editor context (must be non-NULL). Receives created LVGL objects.
 *
 * @note Does not change @c ctx->dirty or filesystem state.
 * @warning Must be called from the LVGL task context.
 */
static void text_editor_build_screen(text_editor_ctx_t *ctx);

/**
 * @brief Update header/status text based on current path and dirty flag.
 *
 * Displays the current path (or directory when path is empty) and prefixes an
 * asterisk (*) when the buffer has unsaved changes.
 *
 * @param ctx Editor context.
 *
 * @note No-op if @c ctx->status_label is NULL.
 */
static void text_editor_update_header(text_editor_ctx_t *ctx);

/**
 * @brief Show a short status message in the toolbar, with optional error color.
 *
 * Sets the toolbar status label text and tint (normal vs error).
 *
 * @param ctx   Editor context.
 * @param msg   Null-terminated message to show (ignored if NULL).
 * @param error Set true to colorize the text as error; false for neutral.
 *
 * @note Does not modify @c ctx->dirty.
 */
static void text_editor_set_status(text_editor_ctx_t *ctx, const char *msg, bool error);

/**
 * @brief Mark the document as modified and refresh the header.
 *
 * Sets @c ctx->dirty=true unless events are currently suppressed, then calls
 * @ref text_editor_update_header.
 *
 * @param ctx Editor context.
 *
 * @note If @c ctx->suppress_events is true, this call is ignored.
 */
static void text_editor_mark_dirty(text_editor_ctx_t *ctx);

/**
 * @brief Close the editor and return to the previous screen.
 *
 * Unloads the editor screen, clears transient UI (e.g., confirm dialog),
 * resets @c active/@c dirty and invokes the optional close callback.
 *
 * @param ctx            Editor context.
 * @param notify_changed If true, passes whether the filesystem changed
 *                       (i.e., @c ctx->fs_changed) to the close callback.
 *
 * @note This does not free the LVGL objects permanently; the screen can be
 *       reused on next @ref text_editor_open.
 */
static void text_editor_close(text_editor_ctx_t *ctx, bool notify_changed);

/**
 * @brief Show a confirmation dialog (discard/delete) with a custom message.
 *
 * Creates a modal message box with Confirm/Cancel buttons and stores the
 * requested confirmation @p mode for subsequent handling.
 *
 * @param ctx     Editor context.
 * @param mode    Confirmation mode (discard or delete).
 * @param message Message body to display.
 *
 * @note No-op if a confirmation dialog is already open.
 */
static void text_editor_confirm_dialog(text_editor_ctx_t *ctx, text_editor_confirm_t mode, const char *message);

/**
 * @brief Save current editor buffer to storage.
 *
 * Validates and normalizes the filename (auto-append “.txt” when needed),
 * composes the final path, writes the content, updates the directory/path,
 * clears the dirty flag and sets @c fs_changed=true.
 *
 * @param ctx Editor context.
 *
 * @retval ESP_OK              On success.
 * @retval ESP_ERR_INVALID_ARG Invalid or unsafe filename.
 * @retval ESP_ERR_INVALID_SIZE Path too long for output buffer.
 * @retval Other               Propagated @c fs_text_write error codes.
 *
 * @note If the path changed (rename), the previous file is deleted.
 * @warning Must be called in a context safe for filesystem access.
 */
static esp_err_t text_editor_save_internal(text_editor_ctx_t *ctx);

/**
 * @brief Delete the current file from storage and update editor state.
 *
 * Clears @c path, resets @c dirty and marks @c fs_changed=true on success.
 *
 * @param ctx Editor context.
 *
 * @retval ESP_OK               File deleted.
 * @retval ESP_ERR_INVALID_STATE No file is currently open.
 * @retval Other                Propagated @c fs_text_delete error codes.
 */
static esp_err_t text_editor_delete_internal(text_editor_ctx_t *ctx);

/**
 * @brief Toggle on-screen keyboard visibility based on focus state.
 *
 * Shows the keyboard when either the filename or the body text area is focused;
 * hides it otherwise and detaches its target textarea.
 *
 * @param ctx Editor context.
 *
 * @note Safe to call repeatedly; does nothing if widgets are missing.
 */
static void text_editor_update_keyboard_visibility(text_editor_ctx_t *ctx);

/**
 * @brief Ensure a name ends with the “.txt” extension.
 *
 * If the name has no extension or ends with a trailing dot, appends “txt”.
 * If the name already ends with “.txt” (case-insensitive), leaves it unchanged.
 *
 * @param[in,out] name Buffer holding the filename to normalize.
 * @param len          Total capacity of @p name (in bytes).
 *
 * @note No-op if the buffer is NULL/empty or too small for the suffix.
 */
static void text_editor_ensure_txt_extension(char *name, size_t len);

/**
 * @brief Handle the Back button click.
 *
 * If there are unsaved changes, opens a discard confirmation dialog; otherwise
 * closes the editor immediately.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_back(lv_event_t *e);

/**
 * @brief Handle the Save button click.
 *
 * Invokes @ref text_editor_save_internal and updates the status label.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_save(lv_event_t *e);

/**
 * @brief Handle the Delete button click.
 *
 * If a file path is present, opens a delete confirmation dialog. Otherwise
 * shows a status message instructing to save before deleting.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_delete(lv_event_t *e);

/**
 * @brief Handle confirmation dialog buttons (Confirm / Cancel).
 *
 * Executes the pending action stored in @c ctx->confirm_mode:
 *  - DISCARD: close without saving
 *  - DELETE : delete file and close on success
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_dialog_button(lv_event_t *e);

/**
 * @brief Value-changed handler for filename/body inputs.
 *
 * Marks the editor as dirty unless events are suppressed.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_value_changed(lv_event_t *e);

/**
 * @brief Focus handler for inputs—shows the on-screen keyboard.
 *
 * Sets the keyboard’s target to the focused widget and makes it visible.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_focused(lv_event_t *e);

/**
 * @brief Defocus handler for inputs—updates keyboard visibility.
 *
 * When neither input is focused, hides the keyboard and detaches its target.
 *
 * @param e LVGL event object (user data must be @c text_editor_ctx_t*).
 */
static void text_editor_on_defocused(lv_event_t *e);

/**
 * @brief Get the base filename from the editor’s current path.
 *
 * @param ctx Editor context.
 * @return Pointer into @c ctx->path at the filename component, or
 *         @c ctx->path itself when no directory separator is present.
 *
 * @note Returned pointer is owned by @c ctx and remains valid while @c path
 *       is unchanged.
 */
static const char *text_editor_filename(const text_editor_ctx_t *ctx)
{
    const char *slash = strrchr(ctx->path, '/');
    return (slash && slash[1]) ? slash + 1 : ctx->path;
}

/**
 * @brief Extract the directory portion of a path into a destination buffer.
 *
 * Copies everything up to (but not including) the last '/' into @p dst.
 * If no '/' exists, writes "/" to @p dst. The result is always NUL-terminated.
 *
 * @param[out] dst     Destination buffer for the directory string.
 * @param dst_len      Size of @p dst in bytes.
 * @param[in]  path    Full path (may be NULL).
 *
 * @note If the directory length would exceed @p dst_len - 1, it is truncated.
 */
static void text_editor_copy_directory(char *dst, size_t dst_len, const char *path)
{
    if (!path || dst_len == 0) {
        return;
    }
    const char *slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    if (len > 0) {
        memcpy(dst, path, len);
        dst[len] = '\0';
    } else {
        strlcpy(dst, "/", dst_len);
    }
}

esp_err_t text_editor_open(const text_editor_open_opts_t *opts)
{
    if (!opts || !opts->return_screen) {
        return ESP_ERR_INVALID_ARG;
    }

    text_editor_ctx_t *ctx = &s_editor;
    memset(ctx, 0, sizeof(*ctx));
    ctx->return_screen = opts->return_screen;
    ctx->close_cb = opts->on_close;
    ctx->close_ctx = opts->user_ctx;
    ctx->confirm_mode = TEXT_EDITOR_CONFIRM_NONE;

    char *loaded = NULL;
    esp_err_t err = ESP_OK;

    if (opts->path && opts->path[0]) {
        if (!fs_text_is_txt(opts->path)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(ctx->path, opts->path, sizeof(ctx->path));
        text_editor_copy_directory(ctx->directory, sizeof(ctx->directory), opts->path);
        err = fs_text_read(opts->path, &loaded, NULL);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        if (!opts->directory || opts->directory[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(ctx->directory, opts->directory, sizeof(ctx->directory));
    }

    if (!ctx->screen) {
        text_editor_build_screen(ctx);
    }

    ctx->suppress_events = true;
    const char *name = opts->path ? text_editor_filename(ctx)
                                  : (opts->suggested_name ? opts->suggested_name : "new_file.txt");
    lv_textarea_set_text(ctx->name_input, name);
    lv_textarea_set_one_line(ctx->name_input, true);
    lv_obj_set_width(ctx->name_input, LV_PCT(100));
    lv_obj_set_user_data(ctx->name_input, ctx);

    lv_textarea_set_text(ctx->body_input, loaded ? loaded : "");
    ctx->suppress_events = false;
    free(loaded);

    ctx->dirty = false;
    ctx->fs_changed = false;
    ctx->active = true;

    text_editor_set_status(ctx, opts->path ? "Loaded" : "New file", false);
    text_editor_update_header(ctx);

    lv_keyboard_set_textarea(ctx->keyboard, ctx->body_input);
    lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ctx->body_input, text_editor_on_value_changed, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(ctx->body_input, text_editor_on_focused, LV_EVENT_FOCUSED, ctx);
    lv_obj_add_event_cb(ctx->body_input, text_editor_on_defocused, LV_EVENT_DEFOCUSED, ctx);
    lv_obj_add_event_cb(ctx->name_input, text_editor_on_value_changed, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(ctx->name_input, text_editor_on_focused, LV_EVENT_FOCUSED, ctx);
    lv_obj_add_event_cb(ctx->name_input, text_editor_on_defocused, LV_EVENT_DEFOCUSED, ctx);

    lv_obj_set_scroll_dir(ctx->screen, LV_DIR_VER);
    lv_screen_load(ctx->screen);
    text_editor_update_keyboard_visibility(ctx);
    return ESP_OK;
}

static void text_editor_build_screen(text_editor_ctx_t *ctx)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_gap(scr, 6, 0);
    ctx->screen = scr;

    lv_obj_t *toolbar = lv_obj_create(scr);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(toolbar, 8, 0);
    ctx->toolbar = toolbar;

    lv_obj_t *btn = lv_button_create(toolbar);
    lv_obj_add_event_cb(btn, text_editor_on_back, LV_EVENT_CLICKED, ctx);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");

    btn = lv_button_create(toolbar);
    lv_obj_add_event_cb(btn, text_editor_on_save, LV_EVENT_CLICKED, ctx);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_SAVE " Save");

    btn = lv_button_create(toolbar);
    lv_obj_add_event_cb(btn, text_editor_on_delete, LV_EVENT_CLICKED, ctx);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_TRASH " Delete");

    ctx->status_label = lv_label_create(toolbar);
    lv_obj_set_flex_grow(ctx->status_label, 1);
    lv_label_set_text(ctx->status_label, "");

    lv_obj_t *name_box = lv_obj_create(scr);
    lv_obj_remove_style_all(name_box);
    lv_obj_set_style_pad_all(name_box, 0, 0);
    lv_obj_set_layout(name_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(name_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(name_box, 4, 0);

    lbl = lv_label_create(name_box);
    lv_label_set_text(lbl, "File name");

    ctx->name_input = lv_textarea_create(name_box);
    lv_textarea_set_one_line(ctx->name_input, true);
    lv_textarea_set_max_length(ctx->name_input, 96);

    ctx->body_input = lv_textarea_create(scr);
    lv_textarea_set_placeholder_text(ctx->body_input, "Start typing...");
    lv_obj_set_flex_grow(ctx->body_input, 1);
    lv_textarea_set_cursor_click_pos(ctx->body_input, true);
    lv_textarea_set_max_length(ctx->body_input, FS_TEXT_MAX_BYTES - 1);

    ctx->keyboard = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(ctx->keyboard, ctx->body_input);
    lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void text_editor_update_header(text_editor_ctx_t *ctx)
{
    if (!ctx->status_label) {
        return;
    }
    const char *path = ctx->path[0] ? ctx->path : ctx->directory;
    lv_label_set_text_fmt(ctx->status_label, "%s%s", ctx->dirty ? "*" : "", path ? path : "-");
}

static void text_editor_set_status(text_editor_ctx_t *ctx, const char *msg, bool error)
{
    if (!ctx->status_label || !msg) {
        return;
    }
    lv_obj_set_style_text_color(ctx->status_label,
                                error ? lv_color_hex(0xff6b6b) : lv_color_hex(0xcfd8dc),
                                0);
    lv_label_set_text(ctx->status_label, msg);
}

static void text_editor_mark_dirty(text_editor_ctx_t *ctx)
{
    if (!ctx || ctx->suppress_events) {
        return;
    }
    ctx->dirty = true;
    text_editor_update_header(ctx);
}

static bool text_editor_validate_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    return fs_text_is_txt(name);
}

static void text_editor_ensure_txt_extension(char *name, size_t len)
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

static esp_err_t text_editor_compose_path(text_editor_ctx_t *ctx, const char *name, char *out, size_t out_len)
{
    if (!name || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->directory[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int needed = snprintf(out, out_len, "%s/%s", ctx->directory, name);
    if (needed < 0 || needed >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t text_editor_save_internal(text_editor_ctx_t *ctx)
{
    const char *raw_name = lv_textarea_get_text(ctx->name_input);
    const char *name_in = raw_name ? raw_name : "";
    char name_buf[FS_NAV_MAX_NAME];
    strlcpy(name_buf, name_in, sizeof(name_buf));
    text_editor_ensure_txt_extension(name_buf, sizeof(name_buf));
    if (strcmp(name_in, name_buf) != 0) {
        ctx->suppress_events = true;
        lv_textarea_set_text(ctx->name_input, name_buf);
        ctx->suppress_events = false;
    }
    if (!text_editor_validate_name(name_buf)) {
        text_editor_set_status(ctx, "Invalid .txt file name", true);
        return ESP_ERR_INVALID_ARG;
    }
    const char *content = lv_textarea_get_text(ctx->body_input);
    if (!content) {
        content = "";
    }

    char target[FS_TEXT_MAX_PATH];
    esp_err_t err = text_editor_compose_path(ctx, name_buf, target, sizeof(target));
    if (err != ESP_OK) {
        text_editor_set_status(ctx, "Path too long", true);
        return err;
    }

    err = fs_text_write(target, content, strlen(content));
    if (err != ESP_OK) {
        text_editor_set_status(ctx, "Failed to save", true);
        return err;
    }

    if (ctx->path[0] && strcmp(ctx->path, target) != 0) {
        fs_text_delete(ctx->path);
    }

    strlcpy(ctx->path, target, sizeof(ctx->path));
    text_editor_copy_directory(ctx->directory, sizeof(ctx->directory), ctx->path);
    ctx->dirty = false;
    ctx->fs_changed = true;
    text_editor_set_status(ctx, "Saved", false);
    text_editor_update_header(ctx);
    return ESP_OK;
}

static void text_editor_close(text_editor_ctx_t *ctx, bool notify_changed)
{
    if (!ctx->return_screen) {
        return;
    }
    lv_screen_load(ctx->return_screen);
    ctx->active = false;
    ctx->dirty = false;
    ctx->confirm_mode = TEXT_EDITOR_CONFIRM_NONE;
    if (ctx->confirm_dialog) {
        lv_msgbox_close(ctx->confirm_dialog);
        ctx->confirm_dialog = NULL;
    }
    if (ctx->close_cb) {
        ctx->close_cb(notify_changed && ctx->fs_changed, ctx->close_ctx);
    }
}

static void text_editor_confirm_dialog(text_editor_ctx_t *ctx, text_editor_confirm_t mode, const char *message)
{
    if (ctx->confirm_dialog) {
        return;
    }
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_obj_set_style_max_width(mbox, LV_PCT(80), 0);
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text(label, message);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, "Confirm");
    lv_obj_set_user_data(btn, (void *)1);
    lv_obj_add_event_cb(btn, text_editor_on_dialog_button, LV_EVENT_CLICKED, ctx);

    btn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_set_user_data(btn, (void *)0);
    lv_obj_add_event_cb(btn, text_editor_on_dialog_button, LV_EVENT_CLICKED, ctx);

    ctx->confirm_dialog = mbox;
    ctx->confirm_mode = mode;
}

static esp_err_t text_editor_delete_internal(text_editor_ctx_t *ctx)
{
    if (!ctx->path[0]) {
        text_editor_set_status(ctx, "Nothing to delete", true);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = fs_text_delete(ctx->path);
    if (err != ESP_OK) {
        text_editor_set_status(ctx, "Delete failed", true);
        return err;
    }
    ctx->path[0] = '\0';
    ctx->dirty = false;
    ctx->fs_changed = true;
    text_editor_set_status(ctx, "Deleted", false);
    return ESP_OK;
}

static void text_editor_on_back(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    if (ctx->dirty) {
        text_editor_confirm_dialog(ctx, TEXT_EDITOR_CONFIRM_DISCARD, "Discard unsaved changes?");
    } else {
        text_editor_close(ctx, ctx->fs_changed);
    }
}

static void text_editor_on_save(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    text_editor_save_internal(ctx);
}

static void text_editor_on_delete(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    if (!ctx->path[0]) {
        text_editor_set_status(ctx, "Save file before deleting", true);
        return;
    }
    text_editor_confirm_dialog(ctx, TEXT_EDITOR_CONFIRM_DELETE, "Delete this file?");
}

static void text_editor_on_dialog_button(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->confirm_dialog) {
        return;
    }
    bool confirm = (bool)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    lv_msgbox_close(ctx->confirm_dialog);
    ctx->confirm_dialog = NULL;

    if (!confirm) {
        ctx->confirm_mode = TEXT_EDITOR_CONFIRM_NONE;
        return;
    }

    switch (ctx->confirm_mode) {
        case TEXT_EDITOR_CONFIRM_DISCARD:
            ctx->dirty = false;
            text_editor_close(ctx, ctx->fs_changed);
            break;
        case TEXT_EDITOR_CONFIRM_DELETE:
            if (text_editor_delete_internal(ctx) == ESP_OK) {
                text_editor_close(ctx, true);
            }
            break;
        default:
            break;
    }
    ctx->confirm_mode = TEXT_EDITOR_CONFIRM_NONE;
}

static void text_editor_on_value_changed(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    text_editor_mark_dirty(ctx);
}

static void text_editor_on_focused(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    if (ctx->keyboard) {
        lv_keyboard_set_textarea(ctx->keyboard, target);
        lv_obj_clear_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void text_editor_on_defocused(lv_event_t *e)
{
    text_editor_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }
    text_editor_update_keyboard_visibility(ctx);
}

static void text_editor_update_keyboard_visibility(text_editor_ctx_t *ctx)
{
    if (!ctx || !ctx->keyboard) {
        return;
    }
    bool focused = lv_obj_has_state(ctx->name_input, LV_STATE_FOCUSED) ||
                   lv_obj_has_state(ctx->body_input, LV_STATE_FOCUSED);
    if (focused) {
        lv_obj_clear_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_keyboard_set_textarea(ctx->keyboard, NULL);
        lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}
