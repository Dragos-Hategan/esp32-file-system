#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_TEXT_MAX_BYTES   (16 * 1024)
#define FS_TEXT_MAX_PATH    512

/**
 * @brief Check whether a file name/path uses a .txt extension (case-insensitive).
 *
 * @param name File name or path.
 * @return true if the string ends with ".txt", false otherwise.
 */
bool fs_text_is_txt(const char *name);

/**
 * @brief Read a text file into a newly-allocated buffer.
 *
 * @param path     Absolute path to the .txt file.
 * @param out_buf  On success, points to a NUL-terminated heap buffer (caller must free).
 * @param out_len  Optional; set to the number of bytes read (excluding NUL).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad params, ESP_ERR_INVALID_SIZE if file
 *         exceeds @ref FS_TEXT_MAX_BYTES, ESP_ERR_NO_MEM on allocation failure, ESP_FAIL on I/O error.
 */
esp_err_t fs_text_read(const char *path, char **out_buf, size_t *out_len);

/**
 * @brief Atomically replace (or create) a text file with the provided buffer.
 *
 * Writes the buffer to a temporary file in the same directory and renames it
 * over @p path. When running on case-insensitive filesystems (e.g., FAT),
 * an existing file is removed before the rename to ensure the case-only rename
 * succeeds.
 *
 * @param path Absolute path to the .txt file.
 * @param data Buffer to write (can be NULL if @p len == 0).
 * @param len  Number of bytes from @p data to write.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad params, ESP_ERR_INVALID_SIZE if path is too long,
 *         ESP_FAIL on filesystem errors.
 */
esp_err_t fs_text_write(const char *path, const char *data, size_t len);

/**
 * @brief Append binary/text data to an existing file (or create if missing).
 *
 * Opens @p path in append mode and writes exactly @p len bytes from @p data
 * to the end of the file. If the file does not exist, a new one is created.
 *
 * @param path Absolute or relative path to the target file.
 * @param data Pointer to the data buffer to append.
 * @param len  Number of bytes to write.
 *
 * @retval ESP_OK              Data appended successfully.
 * @retval ESP_ERR_INVALID_ARG Invalid path or NULL data pointer.
 * @retval ESP_FAIL            File could not be opened or write failed.
 *
 * @note The file is opened in binary mode ("ab" or "wb"), suitable for both
 *       text and raw data. Data is flushed and file closed before returning.
 * @warning If insufficient storage is available, partial writes may occur
 *          before failure is detected.
 */
esp_err_t fs_text_append(const char *path, const char *data, size_t len);

/**
 * @brief Delete a file from the filesystem.
 *
 * Removes the file located at @p path. If the file does not exist,
 * the function logs an error and returns ESP_FAIL.
 *
 * @param path Absolute or relative path to the file to delete.
 *
 * @retval ESP_OK              File successfully removed.
 * @retval ESP_ERR_INVALID_ARG Invalid or unsafe path.
 * @retval ESP_FAIL            File could not be removed (errno reported).
 *
 * @note Uses standard C `remove()` for portability.
 * @warning Fails silently if the path points to a directory instead of a file.
 */
esp_err_t fs_text_delete(const char *path);

/**
 * @brief Create a new empty file.
 *
 * Attempts to create a new file at @p path. If the file already exists,
 * returns ESP_ERR_INVALID_STATE. The file is opened in binary write mode
 * and immediately closed to ensure it exists and is empty.
 *
 * @param path Absolute or relative path to the file to create.
 *
 * @retval ESP_OK               File created successfully.
 * @retval ESP_ERR_INVALID_ARG  Invalid or unsafe path.
 * @retval ESP_ERR_INVALID_STATE File already exists.
 * @retval ESP_FAIL             Creation failed (e.g. permission or I/O error).
 *
 * @note The created file will have zero length.
 */
esp_err_t fs_text_create(const char *path);

#ifdef __cplusplus
}
#endif
