#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Callback invoked when the legacy text editor closes.
 *
 * @param content_changed true if the newly created file was saved.
 * @param user_ctx        User pointer forwarded from @ref text_editor_open_opts_t.
 */
typedef void (*text_editor_close_cb_t)(bool content_changed, void *user_ctx);

/**
 * @brief Configuration used when launching the legacy text editor.
 *
 * Today the dedicated editor is only used for creating new text files. The
 * viewer/editor screen handles all editing of existing files.
 */
typedef struct {
    const char *path;            /**< (Unused) kept for backward compatibility; pass NULL. */
    const char *directory;       /**< Directory where the new file should be created (required). */
    const char *suggested_name;  /**< Initial name shown in the editor; ".txt" added automatically. */
    lv_obj_t *return_screen;     /**< Screen to restore once the editor closes (required). */
    text_editor_close_cb_t on_close; /**< Optional callback to report save/discard outcome. */
    void *user_ctx;              /**< User context forwarded to @p on_close. */
} text_editor_open_opts_t;

/**
 * @brief Launch the legacy text editor to create a new `.txt` file.
 *
 * The editor presents an empty buffer (optionally pre-filled with @p suggested_name)
 * under the specified @p directory. The caller receives a callback when the user
 * saves or discards the file so the file browser can refresh accordingly.
 *
 * @param[in] opts Editor options (must not be NULL).
 *
 * @retval ESP_OK               Editor opened successfully.
 * @retval ESP_ERR_INVALID_ARG  Missing return screen or directory.
 * @retval ESP_ERR_INVALID_STATE Editor is already active.
 */
esp_err_t text_editor_open(const text_editor_open_opts_t *opts);

#ifdef __cplusplus
}
#endif
