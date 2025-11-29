#include "settings.h"

#include <stddef.h>

#include "bsp/esp-bsp.h"
#include "Domine_14.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "nvs.h"

#include "calibration_xpt2046.h"
#include "touch_xpt2046.h"
#include "sd_card.h"

#define SETTINGS_NVS_NS       "settings"
#define SETTINGS_NVS_ROT_KEY  "rotation_step"
#define SETTINGS_ROTATION_STEPS 4

typedef struct{
    int screen_rotation_step;
}settings_t;

typedef struct{
    bool active;                        /**< True while the settings screen is active */
    lv_obj_t *return_screen;            /**< Screen to return to on close */
    lv_obj_t *screen;                   /**< Root LVGL screen object */
    lv_obj_t *toolbar;                  /**< Toolbar container */


    settings_t settings;
}settings_ctx_t;

static settings_t s_settings;
static settings_ctx_t s_settings_ctx;
static const char *TAG = "settings";

static void settings_build_screen(settings_ctx_t *ctx);
static void settings_on_back(lv_event_t *e);
static void settings_close(settings_ctx_t *ctx);

/**
 * @brief Initialize the Non-Volatile Storage (NVS) flash partition.
 *
 * This function initializes the NVS used for storing persistent configuration
 * and calibration data.  
 * If the NVS partition is full, corrupted, or created with an incompatible SDK version,
 * it will be erased and reinitialized automatically.
 *
 * @return
 * - ESP_OK on successful initialization  
 * - ESP_ERR_NVS_NO_FREE_PAGES if the partition had to be erased  
 * - ESP_ERR_NVS_NEW_VERSION_FOUND if a version mismatch was detected  
 * - Other error codes from @ref nvs_flash_init() if initialization fails
 *
 * @note This function should be called before performing any NVS read/write operations.
 */
static esp_err_t init_nvs(void);

/**
 * @brief Starts the BSP display subsystem and reports the initialization result.
 *
 * This function calls `bsp_display_start()` and converts its boolean return
 * value into an `esp_err_t`.  
 * 
 * @return ESP_OK      Display successfully initialized.
 * @return ESP_FAIL    Display failed to initialize.
 */
static esp_err_t bsp_display_start_result(void);

/**
 * @brief Apply the Domine 14 font as the app-wide default LVGL theme font.
 */
static void apply_default_font_theme(void);

/**
 * @brief Apply the current rotation step to the active LVGL display.
 *
 * Maps @ref s_settings.screen_rotation_step to an LVGL display rotation and sets it,
 * clamping to a valid state if needed. Logs a warning when no display exists.
 */
static void apply_rotation_to_display(void);

/**
 * @brief Load persisted rotation step from NVS into @ref s_settings.
 *
 * Reads @ref SETTINGS_NVS_ROT_KEY from @ref SETTINGS_NVS_NS; keeps the
 * default if the key or namespace is missing or out of range.
 */
static void load_rotation_from_nvs(void);

/**
 * @brief Persist current rotation step to NVS.
 *
 * Writes @ref s_settings.screen_rotation_step to @ref SETTINGS_NVS_ROT_KEY inside
 * @ref SETTINGS_NVS_NS, logging warnings on failure but not aborting flow.
 */
static void persist_rotation_to_nvs(void);

/**
 * @brief Initialize runtime settings defaults.
 */
static void init_settings(void);

/**
 * @brief Rotate the display in 90-degree increments (0 -> 90 -> 180 -> 270 -> 0).
 */
static void settings_rotate_screen(lv_event_t *e);

void starting_routine(void)
{
    /* ----- Init NSV ----- */
    ESP_LOGI(TAG, "Initializing NVS");
    ESP_ERROR_CHECK(init_nvs());

    /* ----- Init Display and LVGL ----- */
    ESP_LOGI(TAG, "Starting bsp for ILI9341 display");
    ESP_ERROR_CHECK(bsp_display_start_result()); 
    ESP_ERROR_CHECK(bsp_display_backlight_on()); 
    apply_default_font_theme();
    init_settings();

    /* ----- Init XPT2046 Touch Driver ----- */
    ESP_LOGI(TAG, "Initializing XPT2046 touch driver");
    ESP_ERROR_CHECK(init_touch()); 

    /* ----- Register Touch Driver To LVGL ----- */
    ESP_LOGI(TAG, "Registering touch driver to LVGL");
    ESP_ERROR_CHECK(register_touch_to_lvgl());

    /* ----- Load XPT2046 Touch Driver Calibration ----- */
    bool calibration_found;
    ESP_LOGI(TAG, "Check for touch driver calibration data");
    load_nvs_calibration(&calibration_found);

    /* ----- Calibration Test ----- */
    ESP_LOGI(TAG, "Start calibration dialog");
    ESP_ERROR_CHECK(calibration_test(calibration_found));

    /* ----- Init SDSPI ----- */
    ESP_LOGI(TAG, "Initializing SDSPI");
    esp_err_t err = init_sdspi();
    if (err != ESP_OK){
        retry_init_sdspi();
    }
}

esp_err_t settings_open_settings(lv_obj_t *return_screen)
{
    settings_ctx_t *ctx = &s_settings_ctx;
    if (!ctx->screen){
        ctx->return_screen = return_screen;
        settings_build_screen(ctx);
    }

    ctx->active = true;
    lv_screen_load(ctx->screen);

    return ESP_OK;
}

static void settings_build_screen(settings_ctx_t *ctx)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 2, 0);
    lv_obj_set_style_pad_gap(scr, 5, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    //lv_obj_add_event_cb(scr, settings_on_screen_clicked, LV_EVENT_CLICKED, ctx);
    ctx->screen = scr;

    lv_obj_t *toolbar = lv_obj_create(scr);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(toolbar, 3, 0);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ctx->toolbar = toolbar;    

    lv_obj_t *back_btn = lv_button_create(toolbar);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_set_style_pad_all(back_btn, 6, 0);    
    lv_obj_add_event_cb(back_btn, settings_on_back, LV_EVENT_CLICKED, ctx);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    lv_obj_t *rotate_button = lv_button_create(toolbar);
    lv_obj_set_style_radius(rotate_button, 6, 0);
    lv_obj_set_style_pad_all(rotate_button, 6, 0);    
    lv_obj_add_event_cb(rotate_button, settings_rotate_screen, LV_EVENT_CLICKED, ctx);
    lv_obj_t *rotate_lbl = lv_label_create(rotate_button);
    lv_label_set_text(rotate_lbl, "Rotate Screen");
    lv_obj_center(rotate_lbl);    
}

static void settings_on_back(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    settings_close(ctx);
}

static void settings_close(settings_ctx_t *ctx)
{
    ctx->active = false;
    if (ctx->return_screen)
    {
        lv_screen_load(ctx->return_screen);
    }    
}

static esp_err_t init_nvs(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }

    return nvs_err;
}

static esp_err_t bsp_display_start_result(void)
{
    if (!bsp_display_start()){
        ESP_LOGE(TAG, "BSP failed to initialize display.");
        return ESP_FAIL;
    } 
    return ESP_OK;
}

static void apply_default_font_theme(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "No LVGL display available; cannot set theme font");
        return;
    }

    lv_theme_t *theme = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        false,
        &Domine_14);

    if (!theme) {
        ESP_LOGW(TAG, "Failed to init LVGL default theme with Domine_14");
        return;
    }

    lv_display_set_theme(disp, theme);

    /* Ensure overlay/system layers also inherit the font (dialogs, prompts, etc.) */
    lv_obj_t *act_scr = lv_display_get_screen_active(disp);
    lv_obj_t *top_layer = lv_display_get_layer_top(disp);
    lv_obj_t *sys_layer = lv_display_get_layer_sys(disp);
    lv_obj_set_style_text_font(act_scr, &Domine_14, 0);
    lv_obj_set_style_text_font(top_layer, &Domine_14, 0);
    lv_obj_set_style_text_font(sys_layer, &Domine_14, 0);
}

static void apply_rotation_to_display(void)
{
    lv_display_t *display = lv_display_get_default();
    if (!display) {
        ESP_LOGW(TAG, "No display available; skip applying rotation");
        return;
    }

    /* Map state index to rotation (0:270, 1:180, 2:90, 3:0). */
    switch (s_settings.screen_rotation_step % SETTINGS_ROTATION_STEPS) {
        case 0: lv_display_set_rotation(display, LV_DISPLAY_ROTATION_270); break;
        case 1: lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180); break;
        case 2: lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);  break;
        case 3: lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);   break;
        default:
            s_settings.screen_rotation_step = SETTINGS_ROTATION_STEPS - 1;
            lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
            break;
    }
}

static void load_rotation_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for rotation: %s", esp_err_to_name(err));
        return;
    }

    int32_t stored = s_settings.screen_rotation_step;
    err = nvs_get_i32(h, SETTINGS_NVS_ROT_KEY, &stored);
    nvs_close(h);

    if (err == ESP_OK && stored >= 0 && stored < SETTINGS_ROTATION_STEPS) {
        s_settings.screen_rotation_step = (int)stored;
    }
}

static void persist_rotation_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for rotation: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(h, SETTINGS_NVS_ROT_KEY, s_settings.screen_rotation_step);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save rotation to NVS: %s", esp_err_to_name(err));
    }
}

static void init_settings(void)
{
    /* Default state corresponds to 0-degree rotation (state 3 in our sequence). */
    s_settings.screen_rotation_step = SETTINGS_ROTATION_STEPS - 1;
    load_rotation_from_nvs();
    apply_rotation_to_display();
}

static void settings_rotate_screen(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }

    s_settings.screen_rotation_step = (s_settings.screen_rotation_step + 1) % SETTINGS_ROTATION_STEPS;
    apply_rotation_to_display();
    persist_rotation_to_nvs();
}
