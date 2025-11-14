#include "fs_navigator.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_crc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#define TAG "fs_nav"

#define FS_NAV_STATE_MAGIC 0x464E4156u
#define FS_NAV_NVS_NAMESPACE "fsnav"
#define FS_NAV_NVS_KEY "state_v1"
#define FS_NAV_STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    char relative[FS_NAV_MAX_PATH];
    uint32_t sort_mode;
    uint8_t ascending;
    uint8_t reserved[3];
    uint32_t crc32;
} fs_nav_state_blob_t;

static fs_nav_sort_mode_t s_cmp_mode = FS_NAV_SORT_NAME;
static bool s_cmp_ascending = true;

/**
 * @brief Validate a relative path (no leading '/', no '.' or '..' segments).
 *
 * Empty string is allowed (root). Slash-separated segments must be non-empty and
 * not equal to "." or "..".
 *
 * @param[in] relative Candidate relative path.
 * @return true if valid.
 */
static bool fs_nav_is_valid_relative(const char *relative);

/**
 * @brief Recompute absolute current path from root + relative.
 *
 * @param[in,out] nav Navigator.
 */
static void fs_nav_update_current_path(fs_nav_t *nav);

/**
 * @brief Set @c nav->relative (validated & cleaned) and rebuild @c nav->current.
 *
 * @param[in,out] nav      Navigator.
 * @param[in]     relative New relative path (may be NULL or empty for root).
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if @p relative is invalid
 * - ESP_ERR_INVALID_SIZE if paths exceed buffers
 */
static esp_err_t fs_nav_set_relative(fs_nav_t *nav, const char *relative);

/**
 * @brief Persist current relative path and sort settings to NVS.
 *
 * Computes CRC32 over blob fields and commits.
 *
 * @param[in] nav Navigator.
 * @return
 * - ESP_OK on success
 * - Errors from NVS open/set/commit
 */
static esp_err_t fs_nav_store_state(const fs_nav_t *nav);

/**
 * @brief Load persisted state (relative path and sort) from NVS and validate it.
 *
 * Validates blob size, magic, version, CRC, then applies relative path and sort.
 * If the restored path doesn't exist anymore, resets to root with @c ESP_ERR_NOT_FOUND.
 *
 * @param[in,out] nav Navigator to modify.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_NVS_* / ESP_ERR_INVALID_* on decode/validation failures
 * - ESP_ERR_NOT_FOUND if restored path no longer exists
 */
static esp_err_t fs_nav_load_state(fs_nav_t *nav);

/**
 * @brief Sort the current entries array with current mode and direction.
 *
 * Directories are kept together and sorted by name; files follow the chosen mode.
 *
 * @param[in,out] nav Navigator (no-op for <2 entries or null array).
 */
static void fs_nav_sort_entries(fs_nav_t *nav);

/**
 * @brief qsort comparator for @c fs_nav_entry_t honoring directories-first and sort settings.
 *
 * Directories are always grouped before files. Within directories, sorting is by name to keep
 * navigation intuitive. Files are sorted by current mode (Name/Date/Size). Ties fall back to name.
 *
 * @param lhs Pointer to @c fs_nav_entry_t (left).
 * @param rhs Pointer to @c fs_nav_entry_t (right).
 * @return Negative/zero/positive per strcmp-style semantics; reversed if descending.
 */
static int fs_nav_entry_compare(const void *lhs, const void *rhs);

esp_err_t fs_nav_init(fs_nav_t *nav, const fs_nav_config_t *cfg)
{
    if (!nav || !cfg || !cfg->root_path) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(nav, 0, sizeof(*nav));
    nav->max_entries = cfg->max_entries;
    nav->sort_mode = FS_NAV_SORT_NAME;
    nav->ascending = true;

    size_t root_len = strnlen(cfg->root_path, FS_NAV_MAX_PATH);
    if (root_len == 0 || root_len >= FS_NAV_MAX_PATH) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(nav->root, cfg->root_path, sizeof(nav->root));
    size_t len = strlen(nav->root);
    while (len > 1 && nav->root[len - 1] == '/') {
        nav->root[len - 1] = '\0';
        len--;
    }
    if (nav->root[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = fs_nav_set_relative(nav, "");
    if (err != ESP_OK) {
        return err;
    }

    struct stat st = {0};
    if (stat(nav->current, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Root path \"%s\" not accessible (errno=%d)", nav->current, errno);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t state_err = fs_nav_load_state(nav);
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "Using default navigator state (%s)", esp_err_to_name(state_err));
    }

    err = fs_nav_refresh(nav);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial refresh failed (%s)", esp_err_to_name(err));
    }
    return err;
}

void fs_nav_deinit(fs_nav_t *nav)
{
    if (!nav) {
        return;
    }
    free(nav->entries);
    nav->entries = NULL;
    nav->entry_count = 0;
    nav->capacity = 0;
}

esp_err_t fs_nav_refresh(fs_nav_t *nav)
{
    if (!nav) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(nav->current);
    if (!dir) {
        ESP_LOGE(TAG, "opendir(%s) failed: errno=%d", nav->current, errno);
        nav->entry_count = 0;
        return ESP_FAIL;
    }

    size_t limit = nav->max_entries ? nav->max_entries : SIZE_MAX;
    size_t count = 0;
    struct dirent *dent = NULL;

    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
            continue;
        }
        if (count >= limit) {
            ESP_LOGW(TAG, "Directory listing truncated at %zu entries", limit);
            break;
        }

        if (count == nav->capacity) {
            size_t new_cap = nav->capacity ? nav->capacity * 2 : 32;
            if (nav->max_entries && new_cap > nav->max_entries) {
                new_cap = nav->max_entries;
            }
            fs_nav_entry_t *new_entries = realloc(nav->entries, new_cap * sizeof(fs_nav_entry_t));
            if (!new_entries) {
                closedir(dir);
                ESP_LOGE(TAG, "Out of memory while listing \"%s\"", nav->current);
                return ESP_ERR_NO_MEM;
            }
            nav->entries = new_entries;
            nav->capacity = new_cap;
        }

        fs_nav_entry_t *dest = &nav->entries[count];
        memset(dest, 0, sizeof(*dest));
        strlcpy(dest->name, dent->d_name, sizeof(dest->name));

        char full_path[FS_NAV_MAX_PATH * 2];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", nav->current, dent->d_name);
        if (written <= 0 || (size_t)written >= sizeof(full_path)) {
            ESP_LOGW(TAG, "Skipping overly long path entry: %s", dent->d_name);
            continue;
        }

        struct stat st = {0};
        if (stat(full_path, &st) == 0) {
            dest->is_dir = S_ISDIR(st.st_mode);
            dest->size_bytes = st.st_size;
            dest->modified = st.st_mtime;
        } else {
            dest->is_dir = (dent->d_type == DT_DIR);
            dest->size_bytes = 0;
            dest->modified = 0;
        }

        count++;
    }

    closedir(dir);
    nav->entry_count = count;
    fs_nav_sort_entries(nav);
    return ESP_OK;
}

const fs_nav_entry_t *fs_nav_entries(const fs_nav_t *nav, size_t *count)
{
    if (count) {
        *count = nav ? nav->entry_count : 0;
    }
    return nav ? nav->entries : NULL;
}

const char *fs_nav_current_path(const fs_nav_t *nav)
{
    return nav ? nav->current : NULL;
}

const char *fs_nav_relative_path(const fs_nav_t *nav)
{
    return nav ? nav->relative : NULL;
}

bool fs_nav_can_go_parent(const fs_nav_t *nav)
{
    return nav && nav->relative[0] != '\0';
}

esp_err_t fs_nav_enter(fs_nav_t *nav, size_t index)
{
    if (!nav || index >= nav->entry_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const fs_nav_entry_t *entry = &nav->entries[index];
    if (!entry->is_dir) {
        return ESP_ERR_INVALID_STATE;
    }

    char next_relative[FS_NAV_MAX_PATH];
    if (nav->relative[0] == '\0') {
        strlcpy(next_relative, entry->name, sizeof(next_relative));
    } else {
        int written = snprintf(next_relative, sizeof(next_relative), "%s/%s", nav->relative, entry->name);
        if (written <= 0 || (size_t)written >= sizeof(next_relative)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    esp_err_t err = fs_nav_set_relative(nav, next_relative);
    if (err != ESP_OK) {
        return err;
    }

    err = fs_nav_refresh(nav);
    if (err == ESP_OK) {
        fs_nav_store_state(nav);
    }
    return err;
}

esp_err_t fs_nav_go_parent(fs_nav_t *nav)
{
    if (!fs_nav_can_go_parent(nav)) {
        return ESP_ERR_INVALID_STATE;
    }

    char *slash = strrchr(nav->relative, '/');
    if (slash) {
        *slash = '\0';
    } else {
        nav->relative[0] = '\0';
    }

    esp_err_t err = fs_nav_set_relative(nav, nav->relative);
    if (err != ESP_OK) {
        return err;
    }

    err = fs_nav_refresh(nav);
    if (err == ESP_OK) {
        fs_nav_store_state(nav);
    }
    return err;
}

esp_err_t fs_nav_set_sort(fs_nav_t *nav, fs_nav_sort_mode_t mode, bool ascending)
{
    if (!nav || mode >= FS_NAV_SORT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nav->sort_mode = mode;
    nav->ascending = ascending;
    fs_nav_sort_entries(nav);
    return fs_nav_store_state(nav);
}

fs_nav_sort_mode_t fs_nav_get_sort(const fs_nav_t *nav)
{
    return nav ? nav->sort_mode : FS_NAV_SORT_NAME;
}

bool fs_nav_is_sort_ascending(const fs_nav_t *nav)
{
    return nav ? nav->ascending : true;
}

esp_err_t fs_nav_compose_path(const fs_nav_t *nav, const char *entry_name, char *out, size_t out_len)
{
    if (!nav || !entry_name || !out || out_len == 0 || entry_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int needed = snprintf(out, out_len, "%s/%s", nav->current, entry_name);
    if (needed < 0 || needed >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool fs_nav_is_valid_relative(const char *relative)
{
    if (!relative || relative[0] == '\0') {
        return true;
    }

    const char *p = relative;
    while (*p == '/') {
        p++;
    }

    while (*p) {
        const char *segment_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t len = p - segment_start;
        if (len == 0) {
            return false;
        }
        if ((len == 1 && segment_start[0] == '.') ||
            (len == 2 && segment_start[0] == '.' && segment_start[1] == '.')) {
            return false;
        }
        if (*p == '/') {
            p++;
        }
    }
    return true;
}

static void fs_nav_update_current_path(fs_nav_t *nav)
{
    if (nav->relative[0] == '\0') {
        strlcpy(nav->current, nav->root, sizeof(nav->current));
    } else {
        strlcpy(nav->current, nav->root, sizeof(nav->current));
        strlcat(nav->current, "/", sizeof(nav->current));
        strlcat(nav->current, nav->relative, sizeof(nav->current));
    }
}

static esp_err_t fs_nav_set_relative(fs_nav_t *nav, const char *relative)
{
    const char *clean = relative ? relative : "";
    while (*clean == '/') {
        clean++;
    }

    if (!fs_nav_is_valid_relative(clean)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t rel_len = strlen(clean);
    if (rel_len >= sizeof(nav->relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t root_len = strlen(nav->root);
    size_t needed = root_len + (rel_len ? 1 : 0) + rel_len;
    if (needed >= sizeof(nav->current)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (rel_len == 0) {
        nav->relative[0] = '\0';
    } else {
        strlcpy(nav->relative, clean, sizeof(nav->relative));
    }

    fs_nav_update_current_path(nav);
    return ESP_OK;
}

static esp_err_t fs_nav_store_state(const fs_nav_t *nav)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FS_NAV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    fs_nav_state_blob_t blob = {
        .magic = FS_NAV_STATE_MAGIC,
        .version = FS_NAV_STATE_VERSION,
        .sort_mode = nav->sort_mode,
        .ascending = nav->ascending ? 1 : 0,
    };
    strlcpy(blob.relative, nav->relative, sizeof(blob.relative));
    blob.crc32 = esp_crc32_le(0, (const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc32));

    err = nvs_set_blob(handle, FS_NAV_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t fs_nav_load_state(fs_nav_t *nav)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FS_NAV_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    fs_nav_state_blob_t blob = {0};
    size_t blob_size = sizeof(blob);
    err = nvs_get_blob(handle, FS_NAV_NVS_KEY, &blob, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (blob_size != sizeof(blob)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (blob.magic != FS_NAV_STATE_MAGIC || blob.version != FS_NAV_STATE_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    uint32_t crc = esp_crc32_le(0, (const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc32));
    if (crc != blob.crc32) {
        return ESP_ERR_INVALID_CRC;
    }

    blob.relative[sizeof(blob.relative) - 1] = '\0';

    if (fs_nav_is_valid_relative(blob.relative)) {
        if (fs_nav_set_relative(nav, blob.relative) != ESP_OK) {
            fs_nav_set_relative(nav, "");
        }
    } else {
        fs_nav_set_relative(nav, "");
        return ESP_ERR_INVALID_ARG;
    }

    if (blob.sort_mode < FS_NAV_SORT_COUNT) {
        nav->sort_mode = blob.sort_mode;
    }
    nav->ascending = blob.ascending != 0;

    struct stat st = {0};
    if (stat(nav->current, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fs_nav_set_relative(nav, "");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void fs_nav_sort_entries(fs_nav_t *nav)
{
    if (!nav || nav->entry_count < 2 || !nav->entries) {
        return;
    }
    s_cmp_mode = nav->sort_mode;
    s_cmp_ascending = nav->ascending;
    qsort(nav->entries, nav->entry_count, sizeof(fs_nav_entry_t), fs_nav_entry_compare);
}

static int fs_nav_entry_compare(const void *lhs, const void *rhs)
{
    const fs_nav_entry_t *a = lhs;
    const fs_nav_entry_t *b = rhs;

    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }

    int cmp = 0;
    fs_nav_sort_mode_t mode = a->is_dir ? FS_NAV_SORT_NAME : s_cmp_mode;
    switch (mode) {
        case FS_NAV_SORT_DATE:
            if (a->modified == b->modified) {
                cmp = 0;
            } else {
                cmp = (a->modified < b->modified) ? -1 : 1;
            }
            break;
        case FS_NAV_SORT_SIZE:
            if (a->size_bytes == b->size_bytes) {
                cmp = 0;
            } else {
                cmp = (a->size_bytes < b->size_bytes) ? -1 : 1;
            }
            break;
        case FS_NAV_SORT_NAME:
        default:
            cmp = strcasecmp(a->name, b->name);
            break;
    }

    if (cmp == 0) {
        cmp = strcasecmp(a->name, b->name);
    }
    return s_cmp_ascending ? cmp : -cmp;
}