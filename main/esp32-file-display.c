#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lv_demos.h"
#include "lv_examples.h"
#include "bsp/esp-bsp.h"

#include "calibration_xpt2046.h"
#include "touch_xpt2046.h"

#include "file_browser.h"

static char *TAG = "app_main";

#define LOG_MEM_INFO    (0)

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

static void my_task_function(void *arg)
{
    ESP_LOGI(TAG, "LVGL File Display");

    UBaseType_t freeStack[10];
    freeStack[0] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init NVS ----- */
    ESP_ERROR_CHECK(init_nvs());
    freeStack[1] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init SDSPI ----- */
    init_sdspi();
    freeStack[2] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init Display and LVGL ----- */
    bsp_display_start(); 
    ESP_ERROR_CHECK(bsp_display_backlight_on()); 
    freeStack[3] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Init XPT2046 Touch Driver ----- */
    ESP_ERROR_CHECK(init_touch()); 
    freeStack[4] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Load XPT2046 Touch Driver Calibration ----- */
    bool calibration_found;
    load_nvs_calibration(&calibration_found);
    freeStack[5] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Register Touch Driver To LVGL ----- */
    if (!register_touch_to_lvgl()){
        ESP_LOGE("Touch Driver Registration", "XPT2046 FAILED");
        return;
    }
    freeStack[6] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Calibration Test ----- */
    calibration_test(calibration_found);
    freeStack[7] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Launch File Browser ----- */
    file_browser_config_t browser_cfg = {
        .root_path = SDSPI_MOUNT_POINT,
        .max_entries = 512,
    };
    esp_err_t fb_err = file_browser_start(&browser_cfg);
    if (fb_err != ESP_OK) {
        ESP_LOGE(TAG, "File browser failed to start: %s", esp_err_to_name(fb_err));
    }
    freeStack[8] = uxTaskGetStackHighWaterMark(NULL);

    /* ----- Print Available Memory ----- */
    for (int i = 0; i < 10; i++){
        printf("Free stack[%d]: %d words (%d bytes)\n", i, freeStack[i], freeStack[i] * sizeof(StackType_t));
    }

    vTaskDelay(portMAX_DELAY);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(my_task_function, "MyTask", 12 * 1024, NULL, 1, NULL, 0);

    while (1){
        // size_t free_ram[5];
        // free_ram[0] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        // free_ram[1] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // free_ram[2] = heap_caps_get_free_size(MALLOC_CAP_DMA);

        // heap_caps_print_heap_info(0);  // 0 = all caps
        // printf("\n Free SPIRAM= %u | Free SRAM= %u | Free DMA_RAM= %u\n\n\n\n", free_ram[0], free_ram[1], free_ram[2]);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
