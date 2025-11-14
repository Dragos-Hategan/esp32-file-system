#include "fs_text_ops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "fs_text";

/**
 * @brief Write data atomically by using a temporary file and rename().
 *
 * The function writes @p data to a temporary file in the same directory as
 * @p path, flushes and closes it, then renames the temporary file over the
 * destination. This minimizes the risk of ending up with a partially written
 * file if a failure occurs during the write.
 *
 * Steps:
 *  1) Derive the directory of @p path (or "." if none).
 *  2) Build "<dir>/tmpwrt.tmp" and remove any stale temp file.
 *  3) Write @p len bytes to the temp file and fflush()/fclose().
 *  4) rename(temp, path). If EEXIST, attempt remove(path) then rename again.
 *
 * @param path Destination file path to replace atomically.
 * @param data Buffer with data to write.
 * @param len  Number of bytes in @p data.
 *
 * @retval ESP_OK               On success (destination is replaced).
 * @retval ESP_ERR_INVALID_SIZE Directory or temp-path would overflow buffers.
 * @retval ESP_FAIL             fopen/fwrite/rename or cleanup failed.
 *
 * @note Uses a fixed temp name "tmpwrt.tmp" in the target directory to keep
 *       the rename on the same filesystem. This avoids cross-FS renames.
 * @warning Atomicity semantics depend on the underlying VFS/filesystem. On
 *          POSIX systems, rename() within the same directory is atomic; on
 *          embedded filesystems (e.g., FAT/SD, SPIFFS, LittleFS) behavior
 *          may vary, especially across power loss.
 */
static esp_err_t fs_text_write_atomic(const char *path, const char *data, size_t len);

/**
 * @brief Validate a candidate text-file path against module constraints.
 *
 * Checks that @p path is non-NULL, passes @ref fs_text_is_txt (e.g., extension),
 * and its length is within @c FS_TEXT_MAX_PATH.
 *
 * @param path Path string to validate.
 * @return true  if the path is acceptable for text operations.
 * @return false if the path is NULL, not a .txt (per policy), or too long.
 */
static bool fs_text_check_path(const char *path);

bool fs_text_is_txt(const char *name)
{
    if (!name) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".txt") == 0;
}

esp_err_t fs_text_create(const char *path)
{
    if (!fs_text_check_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(path, &st) == 0) {
        return ESP_ERR_INVALID_STATE; // Already exists
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "create fopen(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

esp_err_t fs_text_read(const char *path, char **out_buf, size_t *out_len)
{
    if (!out_buf || !fs_text_check_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "stat(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    if ((size_t)st.st_size > FS_TEXT_MAX_BYTES) {
        ESP_LOGE(TAG, "File %s too large (%ld bytes)", path, st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }

    size_t size = (size_t)st.st_size;
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, size, f);
    if (read != size && ferror(f)) {
        ESP_LOGE(TAG, "fread(%s) failed (errno=%d)", path, errno);
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    buf[read] = '\0';

    fclose(f);
    *out_buf = buf;
    if (out_len) {
        *out_len = read;
    }
    return ESP_OK;
}

esp_err_t fs_text_write(const char *path, const char *data, size_t len)
{
    if (!fs_text_check_path(path) || (!data && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!data) {
        len = 0;
    }
    return fs_text_write_atomic(path, data, len);
}

esp_err_t fs_text_append(const char *path, const char *data, size_t len)
{
    if (!fs_text_check_path(path) || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "ab");
    if (!f) {
        /* Try to create the file if it doesn't exist */
        f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "fopen(%s) failed (errno=%d)", path, errno);
            return ESP_FAIL;
        }
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "append fwrite(%s) failed (errno=%d)", path, errno);
        fclose(f);
        return ESP_FAIL;
    }
    fflush(f);
    fclose(f);
    return ESP_OK;
}

esp_err_t fs_text_delete(const char *path)
{
    if (!fs_text_check_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "remove(%s) failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t fs_text_write_atomic(const char *path, const char *data, size_t len)
{
    char dir[FS_TEXT_MAX_PATH];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        if (dir_len == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else if (dir_len < sizeof(dir)) {
            memcpy(dir, path, dir_len);
            dir[dir_len] = '\0';
        } else {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        strlcpy(dir, ".", sizeof(dir));
    }

    char tmp_path[FS_TEXT_MAX_PATH];
    int needed = snprintf(tmp_path, sizeof(tmp_path), "%s/tmpwrt.tmp", dir);
    if (needed < 0 || needed >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    remove(tmp_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed (errno=%d)", tmp_path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "fwrite(%s) failed (errno=%d)", tmp_path, errno);
        fclose(f);
        remove(tmp_path);
        return ESP_FAIL;
    }
    fflush(f);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        if (errno == EEXIST) {
            if (remove(path) == 0 && rename(tmp_path, path) == 0) {
                return ESP_OK;
            }
        }
        ESP_LOGE(TAG, "rename(%s -> %s) failed (errno=%d)", tmp_path, path, errno);
        remove(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool fs_text_check_path(const char *path)
{
    if (!path || !fs_text_is_txt(path)) {
        return false;
    }
    size_t len = strnlen(path, FS_TEXT_MAX_PATH + 1);
    return len > 0 && len < FS_TEXT_MAX_PATH;
}