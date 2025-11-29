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

#define SETTINGS_NVS_NS                 "settings"
#define SETTINGS_NVS_ROT_KEY            "rotation_step"
#define SETTINGS_NVS_BRIGHTNESS_KEY     "brightness_pct"
#define SETTINGS_ROTATION_STEPS         4

typedef struct{
    int screen_rotation_step;
    int brightness;
    int saved_brightness;
}settings_t;

typedef struct{
    bool active;                        /**< True while the settings screen is active */
    lv_obj_t *return_screen;            /**< Screen to return to on close */
    lv_obj_t *screen;                   /**< Root LVGL screen object */
    lv_obj_t *toolbar;                  /**< Toolbar container */
    lv_obj_t *brightness_label;         /**< Label showing current brightness percent */
    lv_obj_t *brightness_slider;        /**< Slider to pick brightness percent */
    lv_obj_t * restart_confirm_mbox;    /**<  */
    settings_t settings;                /**< Information about the current session */
}settings_ctx_t;

static settings_t s_settings;
static settings_ctx_t s_settings_ctx;
static const char *TAG = "settings";

/**
 * @brief Build the settings screen (header + scrollable settings list).
 *
 * Creates the root screen, toolbar (Back/About), and the scrollable list of settings.
 *
 * @param ctx Active settings context.
 */
static void settings_build_screen(settings_ctx_t *ctx);

/**
 * @brief Show the About overlay with setting descriptions.
 *
 * Opens a modal overlay on the top layer with descriptive labels and an OK button.
 *
 * @param e LVGL event (CLICKED) with user data = settings_ctx_t*.
 */
static void settings_on_about(lv_event_t *e);

/**
 * @brief Close handler for the About overlay.
 *
 * Deletes the overlay provided via event user data.
 *
 * @param e LVGL event (CLICKED) with user data = overlay obj.
 */
static void settings_on_about_close(lv_event_t *e);

/**
 * @brief Update brightness level when the slider value changes.
 *
 * @param e LVGL event (VALUE_CHANGED) with user data = settings_ctx_t*.
 */
static void settings_on_brightness_changed(lv_event_t *e);

/**
 * @brief Back button handler for the settings screen.
 *
 * Retrieves the settings context from event user data and closes the settings UI.
 *
 * @param e LVGL event (CLICKED) with user data = settings_ctx_t*.
 */
static void settings_on_back(lv_event_t *e);

/**
 * @brief Close the settings screen and restore the previous screen.
 *
 * Marks the context inactive and loads @ref settings_ctx_t::return_screen if set.
 *
 * @param ctx Active settings context.
 */
static void settings_close(settings_ctx_t *ctx);

/**
 * @brief Show a restart confirmation overlay.
 *
 * @param e LVGL event (CLICKED) with user data = settings_ctx_t*.
 */
static void settings_restart(lv_event_t *e);

/**
 * @brief Handler for confirming restart from the overlay.
 *
 * @param e LVGL event (CLICKED) with user data = overlay obj.
 */
static void settings_restart_confirm(lv_event_t *e);

/**
 * @brief Close the restart overlay without restarting.
 *
 * @param e LVGL event (CLICKED) with user data = overlay obj.
 */
static void settings_close_restart(lv_event_t *e);

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
 *
 * @param lock_display True when calling from non-LVGL context (takes display lock);
 *                     false when already in LVGL task (no extra lock).
 */
static void apply_default_font_theme(bool lock_display);

/**
 * @brief Apply the current rotation step to the active LVGL display.
 *
 * Maps @ref s_settings.screen_rotation_step to an LVGL display rotation and sets it,
 * clamping to a valid state if needed. Logs a warning when no display exists.
 *
 * @param lock_display True when calling from non-LVGL context (takes display lock);
 *                     false when already in LVGL task (no extra lock).
 */
static void apply_rotation_to_display(bool lock_display);

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
 * @brief Load persisted brightness percent from NVS (defaults to 100 if missing).
 */
static void load_brightness_from_nvs(void);

/**
 * @brief Persist current brightness percent to NVS.
 */
static void persist_brightness_to_nvs(void);

/**
 * @brief Initialize runtime settings defaults.
 */
static void init_settings(void);

/**
 * @brief Rotate the display in 90-degree increments (0 -> 90 -> 180 -> 270 -> 0).
 * 
 * @param e LVGL event (CLICKED) with user data = settings_ctx_t*.
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
    apply_default_font_theme(true);

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
        settings_build_screen(ctx);
    }

    ctx->active = true;
    ctx->return_screen = return_screen;
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

    lv_obj_t *about_btn = lv_button_create(toolbar);
    lv_obj_set_style_radius(about_btn, 6, 0);
    lv_obj_set_style_pad_all(about_btn, 6, 0);    
    lv_obj_add_event_cb(about_btn, settings_on_about, LV_EVENT_CLICKED, ctx);
    lv_obj_t *about_lbl = lv_label_create(about_btn);
    lv_label_set_text(about_lbl, "About");
    lv_obj_center(about_lbl);    

    /* Scrollable settings list */
    lv_obj_t *settings_list = lv_obj_create(scr);
    lv_obj_remove_style_all(settings_list);
    lv_obj_set_size(settings_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_list,
                          LV_FLEX_ALIGN_CENTER,  /* main axis alignment */
                          LV_FLEX_ALIGN_CENTER,  /* cross axis alignment */
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(settings_list, 1);
    lv_obj_set_scroll_dir(settings_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_top(settings_list, 12, 0);
    lv_obj_set_style_pad_bottom(settings_list, 12, 0);
    lv_obj_set_style_pad_left(settings_list, 12, 0);
    lv_obj_set_style_pad_right(settings_list, 12, 0);
    lv_obj_set_style_pad_row(settings_list, 10, 0);

    lv_obj_t *rotate_button = lv_button_create(settings_list);
    lv_obj_set_width(rotate_button, LV_PCT(100));
    lv_obj_set_style_radius(rotate_button, 8, 0);
    lv_obj_set_style_pad_all(rotate_button, 10, 0);    
    lv_obj_add_event_cb(rotate_button, settings_rotate_screen, LV_EVENT_CLICKED, ctx);
    lv_obj_set_style_align(rotate_button, LV_ALIGN_CENTER, 0);
    lv_obj_t *rotate_lbl = lv_label_create(rotate_button);
    lv_label_set_text(rotate_lbl, "Rotate Screen");
    lv_obj_center(rotate_lbl);    

    lv_obj_t *restart_button = lv_button_create(settings_list);
    lv_obj_set_width(restart_button, LV_PCT(100));
    lv_obj_set_style_radius(restart_button, 8, 0);
    lv_obj_set_style_pad_all(restart_button, 10, 0);    
    lv_obj_add_event_cb(restart_button, settings_restart, LV_EVENT_CLICKED, ctx);
    lv_obj_set_style_align(restart_button, LV_ALIGN_CENTER, 0);
    lv_obj_t *restart_lbl = lv_label_create(restart_button);
    lv_label_set_text(restart_lbl, "Restart");
    lv_obj_center(restart_lbl);

    lv_obj_t *brightness_card = lv_button_create(settings_list);
    lv_obj_set_width(brightness_card, LV_PCT(100));
    lv_obj_set_height(brightness_card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(brightness_card, 10, 0);
    lv_obj_set_style_pad_row(brightness_card, 6, 0);
    lv_obj_set_style_radius(brightness_card, 8, 0);
    lv_obj_set_flex_flow(brightness_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_align(brightness_card, LV_ALIGN_CENTER, 0);
    lv_obj_clear_flag(brightness_card, LV_OBJ_FLAG_CLICKABLE); /* container only */

    ctx->brightness_label = lv_label_create(brightness_card);
    lv_obj_set_width(ctx->brightness_label, LV_PCT(100));
    lv_obj_set_style_text_align(ctx->brightness_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ctx->brightness_label, lv_color_hex(0xe0e0e0), 0);

    ctx->brightness_slider = lv_slider_create(brightness_card);
    lv_obj_set_width(ctx->brightness_slider, LV_PCT(100));
    lv_slider_set_range(ctx->brightness_slider, 0, 100);
    lv_slider_set_value(ctx->brightness_slider, s_settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(ctx->brightness_slider, settings_on_brightness_changed, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_set_style_bg_color(ctx->brightness_slider, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ctx->brightness_slider, lv_palette_main(LV_PALETTE_RED), LV_PART_KNOB);
    lv_obj_set_style_bg_color(ctx->brightness_slider, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->brightness_slider, LV_OPA_COVER, LV_PART_KNOB | LV_PART_INDICATOR | LV_PART_MAIN);

    int init_val = lv_slider_get_value(ctx->brightness_slider);
    char init_txt[32];
    lv_snprintf(init_txt, sizeof(init_txt), "Brightness: %d%%", init_val);
    lv_label_set_text(ctx->brightness_label, init_txt);
}

static void settings_on_about(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_30, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *dlg = lv_obj_create(overlay);
    lv_obj_set_style_radius(dlg, 12, 0);
    lv_obj_set_style_pad_all(dlg, 16, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x202126), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0x3a3d45), 0);
    lv_obj_set_width(dlg, LV_PCT(80));
    lv_obj_set_height(dlg, LV_PCT(90));
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_center(dlg);

    lv_obj_t *list = lv_obj_create(dlg);
    lv_obj_remove_style_all(list);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_row(list, 10, 0);

    const char *lines[] = {
        "Setting 1: this is a mediummedium text about setting 1",
        "Setting 2: this is a short text about setting 2",
        "Setting 3: this is a mediummedium text about setting 3",
        "Setting 4: this is a short text about setting 4",
        "Setting 5: this is a LOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOONG text about setting 5",
        "Setting 6: this is a mediummedium text about setting 6",
        "Setting 7: this is a LOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOONG text about setting 7",
        "Setting 8: this is a LOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOONG text about setting 8",        
    };

    for (size_t i = 0; i < sizeof(lines)/sizeof(lines[0]); i++) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, lines[i]);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xe0e0e0), 0);
    }

    lv_obj_t *ok_btn = lv_button_create(dlg);
    lv_obj_set_width(ok_btn, LV_PCT(100));
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_pad_all(ok_btn, 8, 0);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);

    lv_obj_add_event_cb(ok_btn, settings_on_about_close, LV_EVENT_CLICKED, overlay);
}

static void settings_on_about_close(lv_event_t *e)
{
    lv_obj_t *overlay = lv_event_get_user_data(e);
    if (overlay) {
        lv_obj_del(overlay);
    }
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
    if (ctx && ctx->brightness_slider) {
        int val = lv_slider_get_value(ctx->brightness_slider);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        s_settings.brightness = val;
        if (s_settings.brightness != s_settings.saved_brightness) {
            persist_brightness_to_nvs();
        }
    }

    persist_rotation_to_nvs();

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

static void apply_default_font_theme(bool lock_display)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        ESP_LOGW(TAG, "No LVGL display available; cannot set theme font");
        return;
    }
    
    if (lock_display){
        bsp_display_lock(0);
    }

    lv_theme_t *theme = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        false,
        &Domine_14);

    if (!theme) {
        ESP_LOGW(TAG, "Failed to init LVGL default theme with Domine_14");
        if (lock_display){
            bsp_display_unlock();
        }
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

    if (lock_display){
        bsp_display_unlock();
    }
}

static void apply_rotation_to_display(bool lock_display)
{
    lv_display_t *display = lv_display_get_default();
    if (!display) {
        ESP_LOGW(TAG, "No display available; skip applying rotation");
        return;
    }

    if (lock_display) {
        bsp_display_lock(0);
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

    if (lock_display) {
        bsp_display_unlock();
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

static void load_brightness_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        s_settings.brightness = 100;
        s_settings.saved_brightness = s_settings.brightness;
        return;
    }

    int32_t stored = 100;
    err = nvs_get_i32(h, SETTINGS_NVS_BRIGHTNESS_KEY, &stored);
    nvs_close(h);

    if (err == ESP_OK && stored >= 0 && stored <= 100) {
        s_settings.brightness = (int)stored;
        s_settings.saved_brightness = s_settings.brightness;
    } else {
        s_settings.brightness = 100;
        s_settings.saved_brightness = s_settings.brightness;
    }
}

static void persist_brightness_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for brightness: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(h, SETTINGS_NVS_BRIGHTNESS_KEY, s_settings.brightness);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save brightness to NVS: %s", esp_err_to_name(err));
    } else {
        s_settings.saved_brightness = s_settings.brightness;
    }
}

static void init_settings(void)
{
    /* Default state corresponds to 0-degree rotation (state 3 in our sequence). */
    s_settings.screen_rotation_step = SETTINGS_ROTATION_STEPS - 1;
    s_settings.brightness = 100;
    s_settings.saved_brightness = 100;
    load_brightness_from_nvs();
    load_rotation_from_nvs();
    bsp_display_brightness_set(s_settings.brightness);
    apply_rotation_to_display(true);
}

static void settings_rotate_screen(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }

    s_settings.screen_rotation_step = (s_settings.screen_rotation_step + 1) % SETTINGS_ROTATION_STEPS;
    apply_rotation_to_display(false);
}

static void settings_on_brightness_changed(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->brightness_label || !ctx->brightness_slider) {
        return;
    }

    int val = lv_slider_get_value(ctx->brightness_slider);
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    s_settings.brightness = val;
    char txt[32];
    lv_snprintf(txt, sizeof(txt), "Brightness: %d%%", val);
    lv_label_set_text(ctx->brightness_label, txt);

    bsp_display_brightness_set(val);
}

static void settings_restart(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }

    lv_obj_t *mbox = lv_msgbox_create(NULL);
    ctx->restart_confirm_mbox = mbox;
    lv_obj_set_style_max_width(mbox, LV_PCT(80), 0);
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text_fmt(label, "Are you sure you want to restart?");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *yes_btn = lv_msgbox_add_footer_button(mbox, "Yes");
    lv_obj_set_user_data(yes_btn, (void *)1);
    lv_obj_add_event_cb(yes_btn, settings_restart_confirm, LV_EVENT_CLICKED, ctx);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_set_user_data(cancel_btn, (void *)0);
    lv_obj_add_event_cb(cancel_btn, settings_close_restart, LV_EVENT_CLICKED, ctx);
}

static void settings_restart_confirm(lv_event_t *e)
{
    persist_brightness_to_nvs();
    persist_rotation_to_nvs();
    esp_restart();
}

static void settings_close_restart(lv_event_t *e)
{
    settings_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->restart_confirm_mbox)
    {
        return;
    }    
    if (ctx && ctx->restart_confirm_mbox) {
        lv_msgbox_close(ctx->restart_confirm_mbox);
        ctx->restart_confirm_mbox = NULL;
    }    
}
