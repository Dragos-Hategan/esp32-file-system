#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "calibration_xpt2046.h"
#include "touch_xpt2046.h"
#include "file_browser.h"
#include "sd_card.h"
#include "Domine_14.h"

static char *TAG = "app_main";

#define LOG_MEM_INFO    (0)

/**
 * @brief Starts the BSP display subsystem and reports the initialization result.
 *
 * This function calls `bsp_display_start()` and converts its boolean return
 * value into an `esp_err_t`.  
 * 
 * @return ESP_OK      Display successfully initialized.
 * @return ESP_FAIL    Display failed to initialize.
 */
static esp_err_t bsp_display_start_result(void)
{
    if (!bsp_display_start()){
        ESP_LOGE(TAG, "BSP failed to initialize display.");
        return ESP_FAIL;
    } 
    return ESP_OK;
}

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
static esp_err_t init_nvs(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }

    return nvs_err;
}

/**
 * @brief Apply the Domine 14 font as the app-wide default LVGL theme font.
 */
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

static void main_task(void *arg)
{
    ESP_LOGI(TAG, "\n\n ********** LVGL File Display ********** \n");

    UBaseType_t freeStack[20];
    freeStack[0] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init Display and LVGL ----- */
    ESP_LOGI(TAG, "Starting bsp for ILI9341 display");
    ESP_ERROR_CHECK(bsp_display_start_result()); 
    ESP_ERROR_CHECK(bsp_display_backlight_on()); 
    apply_default_font_theme();
    freeStack[1] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init NVS ----- */
    ESP_LOGI(TAG, "Initializing NVS");
    ESP_ERROR_CHECK(init_nvs());
    freeStack[2] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init XPT2046 Touch Driver ----- */
    ESP_LOGI(TAG, "Initializing XPT2046 touch driver");
    ESP_ERROR_CHECK(init_touch()); 
    freeStack[3] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Register Touch Driver To LVGL ----- */
    ESP_LOGI(TAG, "Registering touch driver to LVGL");
    ESP_ERROR_CHECK(register_touch_to_lvgl());
    freeStack[4] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Load XPT2046 Touch Driver Calibration ----- */
    bool calibration_found;
    ESP_LOGI(TAG, "Check for touch driver calibration data");
    load_nvs_calibration(&calibration_found);
    freeStack[5] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Calibration Test ----- */
    ESP_LOGI(TAG, "Start calibration dialog");
    ESP_ERROR_CHECK(calibration_test(calibration_found));
    freeStack[6] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init SDSPI ----- */
    ESP_LOGI(TAG, "Initializing SDSPI");
    esp_err_t err = init_sdspi();
    if (err != ESP_OK){
        retry_init_sdspi();
    }
    freeStack[7] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Launch File Browser ----- */
    ESP_LOGI(TAG, "Start file browser UI");
    esp_err_t fb_err = file_browser_start();
    if (fb_err != ESP_OK) {
        ESP_LOGE(TAG, "File browser failed to start: %s", esp_err_to_name(fb_err));
        return;
    }
    freeStack[8] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Print Available Memory ----- */
    for (int i = 0; i < 10; i++){
        printf("Free stack[%d]: %d words (%d bytes)\n", i, freeStack[i], freeStack[i] * sizeof(StackType_t));
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(main_task, "MyTask", 8 * 1024, NULL, 1, NULL, 0);

    while (1){
        size_t free_heap = esp_get_free_heap_size();
        size_t min_free_heap = esp_get_minimum_free_heap_size();
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
 
        printf("free=%u  min=%u max=%u\n", free_heap, min_free_heap, largest_block);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
