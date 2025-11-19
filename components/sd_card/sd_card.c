#include "sd_card.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdmmc_cmd.h"

#define SDSPI_RETRY_UI_STEP_MS  50
#define SDSPI_RETRY_DELAY_MS    500
#define SDSPI_MAX_RETRIES       5

static const char* TAG = "sd_card";
static sdmmc_card_t *sd_card_handle = NULL;
static bool sd_spi_bus_ready = false;

typedef struct {
    SemaphoreHandle_t semaphore;
} sdspi_retry_prompt_ctx_t;

typedef struct {
    lv_obj_t *container;
    lv_obj_t *message_label;
    lv_obj_t *attempt_label;
    lv_obj_t *arc;
    uint32_t total_duration_ms;
} sdspi_retry_ui_t;

static void sdspi_retry_prompt_event_cb(lv_event_t *e)
{
    sdspi_retry_prompt_ctx_t *ctx = (sdspi_retry_prompt_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->semaphore) {
        return;
    }

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    xSemaphoreGive(ctx->semaphore);
}

static void sdspi_retry_wait_for_confirmation(void)
{
    sdspi_retry_prompt_ctx_t ctx = {
        .semaphore = xSemaphoreCreateBinary(),
    };
    if (!ctx.semaphore) {
        ESP_LOGW(TAG, "Failed to allocate semaphore for SDSPI retry prompt");
        return;
    }

    if (!bsp_display_lock(0)) {
        vSemaphoreDelete(ctx.semaphore);
        ESP_LOGW(TAG, "Unable to acquire display lock for SDSPI retry prompt");
        return;
    }

    lv_obj_t *layer = lv_layer_top();
    lv_obj_t *mbox = lv_msgbox_create(layer);
    lv_obj_set_style_max_width(mbox, lv_pct(80), 0);
    lv_obj_center(mbox);

    lv_obj_t *label = lv_label_create(mbox);
    lv_label_set_text(label, "Check SD card connection and hit OK");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));

    lv_obj_t *btn = lv_msgbox_add_footer_button(mbox, "OK");
    lv_obj_add_event_cb(btn, sdspi_retry_prompt_event_cb, LV_EVENT_CLICKED, &ctx);

    lv_obj_invalidate(mbox);
    lv_refr_now(NULL);
    bsp_display_unlock();

    if (xSemaphoreTake(ctx.semaphore, portMAX_DELAY) == pdTRUE) {
        ESP_LOGW(TAG, "SDSPI retry prompt wait aborted");
    }

    if (bsp_display_lock(0)) {
        lv_obj_del(mbox);
        bsp_display_unlock();
    }

    vSemaphoreDelete(ctx.semaphore);
}

static void sdspi_retry_ui_set_message(sdspi_retry_ui_t *ui, const char *text)
{
    if (!ui || !ui->container || !ui->message_label || !text) {
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }
    lv_label_set_text(ui->message_label, text);
    bsp_display_unlock();
}

static void sdspi_retry_ui_set_attempt(sdspi_retry_ui_t *ui, uint32_t attempt)
{
    if (!ui || !ui->container || !ui->attempt_label) {
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }
    lv_label_set_text_fmt(ui->attempt_label, "Attempt %lu/%d", attempt, SDSPI_MAX_RETRIES);
    bsp_display_unlock();
}

static void sdspi_retry_ui_set_progress(sdspi_retry_ui_t *ui, uint32_t elapsed_ms)
{
    if (!ui || !ui->container || !ui->arc || ui->total_duration_ms == 0) {
        return;
    }

    if (elapsed_ms > ui->total_duration_ms) {
        elapsed_ms = ui->total_duration_ms;
    }

    if (!bsp_display_lock(0)) {
        return;
    }
    lv_arc_set_value(ui->arc, elapsed_ms);
    bsp_display_unlock();
}

static void sdspi_retry_ui_wait(sdspi_retry_ui_t *ui, uint32_t *elapsed_ms, uint32_t wait_ms)
{
    if (!elapsed_ms) {
        return;
    }

    uint32_t target = *elapsed_ms + wait_ms;

    if (!ui || !ui->container || !ui->arc) {
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        if (ticks == 0) {
            ticks = 1;
        }
        vTaskDelay(ticks);
        *elapsed_ms = target;
        return;
    }

    while (*elapsed_ms < target) {
        uint32_t chunk_ms = target - *elapsed_ms;
        if (chunk_ms > SDSPI_RETRY_UI_STEP_MS) {
            chunk_ms = SDSPI_RETRY_UI_STEP_MS;
        }
        TickType_t ticks = pdMS_TO_TICKS(chunk_ms);
        if (ticks == 0) {
            ticks = 1;
        }
        vTaskDelay(ticks);
        *elapsed_ms += chunk_ms;
        sdspi_retry_ui_set_progress(ui, *elapsed_ms);
    }
}

static void sdspi_retry_ui_destroy(sdspi_retry_ui_t *ui)
{
    if (!ui || !ui->container) {
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }
    lv_obj_del(ui->container);
    bsp_display_unlock();

    ui->container = NULL;
    ui->message_label = NULL;
    ui->attempt_label = NULL;
    ui->arc = NULL;
}

static void sdspi_retry_ui_create(sdspi_retry_ui_t *ui, uint32_t total_duration_ms)
{
    if (!ui) {
        return;
    }

    ui->total_duration_ms = total_duration_ms;

    if (!bsp_display_lock(0)) {
        ESP_LOGW(TAG, "Unable to acquire display lock for SDSPI retry UI");
        return;
    }

    lv_obj_t *parent = lv_layer_top();
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_style_pad_all(container, 16, 0);
    lv_obj_set_style_pad_row(container, 12, 0);
    lv_obj_set_style_radius(container, 12, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x202126), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x3a3d45), 0);
    lv_obj_set_width(container, lv_pct(80));
    lv_obj_set_height(container, LV_SIZE_CONTENT);
    lv_obj_center(container);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(container, 12, 0);
    lv_obj_set_flex_align(container,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *message = lv_label_create(container);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(message, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, lv_pct(100));
    lv_label_set_text(message, "SD card failed, retrying...");

    lv_obj_t *arc = lv_arc_create(container);
    lv_obj_set_size(arc, 100, 100);
    lv_arc_set_range(arc, 0, total_duration_ms);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_center(arc);

    lv_obj_t *attempt = lv_label_create(container);
    lv_obj_set_width(attempt, lv_pct(100));
    lv_obj_set_style_text_align(attempt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(attempt, lv_color_hex(0xa0a0a0), 0);
    lv_label_set_text_fmt(attempt, "Attempt 0/%d", SDSPI_MAX_RETRIES);

    ui->container = container;
    ui->message_label = message;
    ui->attempt_label = attempt;
    ui->arc = arc;

    bsp_display_unlock();
}

sdspi_result_t init_sdspi(void)
{
    const char *TAG_INIT_SDSPI = "init_sdspi";

    sdspi_result_t sdspi_result = {
        .sdspi_failcode = SDSPI_SUCCESS,
        .esp_err = ESP_OK
    };

    if (!sd_spi_bus_ready) {
        ESP_LOGI(TAG_INIT_SDSPI, "Initializing SPI bus");
        spi_bus_config_t spi_bus_config = {
            .mosi_io_num = CONFIG_SDSPI_BUS_MOSI_PIN,
            .miso_io_num = CONFIG_SDSPI_BUS_MISO_PIN,
            .sclk_io_num = CONFIG_SPSPI_BUS_SCL_PIN,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(CONFIG_SDSPI_BUS_HOST, &spi_bus_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_INIT_SDSPI, "Failed to init SDSPI bus: (%s)", esp_err_to_name(err));
            sdspi_result.sdspi_failcode = SDSPI_SPI_BUS_FAILED;
            sdspi_result.esp_err = err;
            return sdspi_result;
        }
        sd_spi_bus_ready = true;
    }

    if (sd_card_handle) {
        ESP_LOGI(TAG_INIT_SDSPI, "SDSPI already mounted at %s", CONFIG_SDSPI_MOUNT_POINT);
        return sdspi_result;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_SDSPI_MAX_FREQ_KHZ;
    host.slot = CONFIG_SDSPI_BUS_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_SDSPI_DEVICE_CS_PIN;
    slot_config.host_id = CONFIG_SDSPI_BUS_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .allocation_unit_size = 16 * 1024,
        .format_if_mount_failed = false,
        .max_files = 5,
    };

    ESP_LOGI(TAG_INIT_SDSPI, "Mounting SDSPI filesystem at %s", CONFIG_SDSPI_MOUNT_POINT);
    esp_err_t err = esp_vfs_fat_sdspi_mount(CONFIG_SDSPI_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_INIT_SDSPI, "Failed to init SD card: (%s). Check wiring/pull-ups.", esp_err_to_name(err));
        sdspi_result.sdspi_failcode = SDSPI_FAT_MOUNT_FAILED;
        sdspi_result.esp_err = err;
        sd_card_handle = NULL;
        return sdspi_result;
    }

    sdmmc_card_print_info(stdout, sd_card_handle);
    ESP_LOGI(TAG_INIT_SDSPI, "SDSPI ready");

    return sdspi_result;
}

static bool retry_init_sdspi(void)
{
    sdspi_retry_wait_for_confirmation();

    const uint32_t total_wait_ms = SDSPI_MAX_RETRIES * SDSPI_RETRY_DELAY_MS;
    sdspi_retry_ui_t retry_ui = {0};
    sdspi_retry_ui_create(&retry_ui, total_wait_ms);

    sdspi_result_t retry_res = {0};
    uint32_t elapsed_ms = 0;

    for (int attempt = 1; attempt <= SDSPI_MAX_RETRIES; attempt++) {
        ESP_LOGW(TAG, "Retrying SDSPI init %d/%d...", attempt, SDSPI_MAX_RETRIES);
        sdspi_retry_ui_set_attempt(&retry_ui, attempt);
        sdspi_retry_ui_wait(&retry_ui, &elapsed_ms, SDSPI_RETRY_DELAY_MS);

        retry_res = init_sdspi();
        if (retry_res.sdspi_failcode == SDSPI_SUCCESS) {
            sdspi_retry_ui_set_message(&retry_ui, "SDSPI recovered");
            sdspi_retry_ui_set_progress(&retry_ui, total_wait_ms);
            sdspi_retry_ui_destroy(&retry_ui);
            ESP_LOGW(TAG, "SDSPI recovered after %d attempt(s)", attempt);
            return true;
        }
    }

    sdspi_retry_ui_set_message(&retry_ui, "SDSPI retry failed");
    sdspi_retry_ui_set_progress(&retry_ui, total_wait_ms);
    sdspi_retry_ui_destroy(&retry_ui);

    ESP_LOGE(TAG, "SDSPI SPI init failed after %d retries. Last ESP err: %s",
             SDSPI_MAX_RETRIES,
             esp_err_to_name(retry_res.esp_err));

    return false;
}

void sdspi_fallback(sdspi_result_t res){
    switch (res.sdspi_failcode) {
        case SDSPI_SPI_BUS_FAILED:
        case SDSPI_FAT_MOUNT_FAILED:
        case SDSPI_COMMUNICATION_FAILED:
            if (!retry_init_sdspi()){
                // esp_restart() or continue without file browser?
            }
            break;

        default:
            ESP_LOGE("sdspi", "Unknown SDSPI error (failcode=%d, esp_err=%s)",
                     res.sdspi_failcode, esp_err_to_name(res.esp_err));
            break;
    }
}
