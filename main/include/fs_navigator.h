// main/fs_nav.h
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_NAV_MAX_PATH 256
#define FS_NAV_MAX_NAME 96

typedef enum {
    FS_NAV_SORT_NAME = 0,
    FS_NAV_SORT_DATE = 1,
    FS_NAV_SORT_SIZE = 2,
    FS_NAV_SORT_COUNT
} fs_nav_sort_mode_t;

typedef struct {
    char name[FS_NAV_MAX_NAME];
    bool is_dir;
    size_t size_bytes;
    time_t modified;
} fs_nav_entry_t;

typedef struct fs_nav {
    char root[FS_NAV_MAX_PATH];
    char current[FS_NAV_MAX_PATH];
    char relative[FS_NAV_MAX_PATH];
    fs_nav_entry_t *entries;
    size_t entry_count;
    size_t capacity;
    size_t max_entries;
    fs_nav_sort_mode_t sort_mode;
    bool ascending;
} fs_nav_t;

typedef struct {
    const char *root_path;
    size_t max_entries;
} fs_nav_config_t;

/**
 * @brief Initialize a navigator rooted at @p cfg->root_path and load persisted state if present.
 *
 * Trims trailing slashes, validates root is an absolute directory, restores last relative path,
 * sort mode and direction from NVS (best effort), then performs an initial directory scan.
 *
 * @param[out] nav Navigator instance to initialize.
 * @param[in]  cfg Configuration (root path and entry cap).
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG on null args or invalid root path
 * - ESP_ERR_NOT_FOUND if root is not accessible
 * - Errors from fs_nav_refresh on initial scan failure
 */
esp_err_t fs_nav_init(fs_nav_t *nav, const fs_nav_config_t *cfg);

/**
 * @brief Release resources held by the navigator (directory entries).
 *
 * @param[in,out] nav Navigator to deinitialize (safe to pass NULL).
 */
void fs_nav_deinit(fs_nav_t *nav);

/**
 * @brief Rescan the current directory and rebuild the entry array (sorted).
 *
 * Applies @c nav->max_entries limit if non-zero and sorts entries according to current mode.
 *
 * @param[in,out] nav Navigator.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if @p nav is NULL
 * - ESP_FAIL if directory cannot be opened
 * - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t fs_nav_refresh(fs_nav_t *nav);

/**
 * @brief Get a pointer to the internal entries array.
 *
 * @param[in]  nav   Navigator.
 * @param[out] count Optional; set to number of valid entries.
 * @return Pointer to internal array (NULL if @p nav is NULL).
 * @warning The pointer becomes invalid after @c fs_nav_refresh or sort changes.
 */
const fs_nav_entry_t *fs_nav_entries(const fs_nav_t *nav, size_t *count);

/**
 * @brief Get absolute current path (root + relative).
 * @param[in] nav Navigator.
 * @return NUL-terminated path or NULL.
 */
const char *fs_nav_current_path(const fs_nav_t *nav);

/**
 * @brief Get relative path from root ('' means root).
 * @param[in] nav Navigator.
 * @return NUL-terminated relative path or NULL.
 */
const char *fs_nav_relative_path(const fs_nav_t *nav);

/**
 * @brief Whether a parent directory exists (i.e., we are not at root).
 * @param[in] nav Navigator.
 * @return true if @p nav is valid and relative path is non-empty.
 */
bool fs_nav_can_go_parent(const fs_nav_t *nav);

/**
 * @brief Enter the directory at @p index in the current listing and refresh.
 *
 * @param[in,out] nav   Navigator.
 * @param[in]     index Index into @c fs_nav_entries.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG for bad args/index
 * - ESP_ERR_INVALID_STATE if selected entry is not a directory
 * - Errors from internal path set/refresh/store
 */
esp_err_t fs_nav_enter(fs_nav_t *nav, size_t index);

/**
 * @brief Go to parent directory (if any) and refresh.
 *
 * @param[in,out] nav Navigator.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if already at root
 * - Errors from internal path set/refresh/store
 */
esp_err_t fs_nav_go_parent(fs_nav_t *nav);

/**
 * @brief Set sort mode and direction, then sort current entries and persist state.
 *
 * @param[in,out] nav       Navigator.
 * @param[in]     mode      Sort mode (Name/Date/Size).
 * @param[in]     ascending true for ascending, false for descending.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if @p nav is NULL or mode is invalid
 * - Errors from state store
 */
esp_err_t fs_nav_set_sort(fs_nav_t *nav, fs_nav_sort_mode_t mode, bool ascending);

/**
 * @brief Get current sort mode.
 * @param[in] nav Navigator.
 * @return Sort mode (Name by default if @p nav is NULL).
 */
fs_nav_sort_mode_t fs_nav_get_sort(const fs_nav_t *nav);

/**
 * @brief Check if sort direction is ascending.
 * @param[in] nav Navigator.
 * @return true if ascending (default true when @p nav is NULL).
 */
bool fs_nav_is_sort_ascending(const fs_nav_t *nav);

/**
 * @brief Compose an absolute path by appending @p entry_name to the current directory.
 *
 * @param[in]  nav         Navigator (must be initialized).
 * @param[in]  entry_name  Child entry name (e.g., from fs_nav_entries()).
 * @param[out] out         Buffer for the resulting absolute path.
 * @param[in]  out_len     Size of @p out.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad inputs, ESP_ERR_INVALID_SIZE if buffer is too small.
 */
esp_err_t fs_nav_compose_path(const fs_nav_t *nav, const char *entry_name, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
