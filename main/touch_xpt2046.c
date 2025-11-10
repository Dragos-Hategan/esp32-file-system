#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "touch_xpt2046.h"
#include "ui.h"
#include "esp_lcd_touch_xpt2046.h"
#include "nvs.h"
#include "esp_log.h"

#include <math.h>

/** @brief 5-point calibration target set (screen-space). */
static cal_point_t CAL_POINT_ARROWS[5] = {
        {20, 20, 0, 0},                                 // top left
        {TOUCH_X_MAX - 20, 20, 0, 0},                   // top right
        {TOUCH_X_MAX - 20, TOUCH_Y_MAX - 20, 0, 0},     // bottom right
        {20, TOUCH_Y_MAX - 20, 0, 0},                   // bottom left
        {TOUCH_X_MAX / 2, TOUCH_Y_MAX / 2, 0, 0}        // center
    };

static esp_lcd_touch_handle_t touch_handle = NULL;

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return touch_handle;
}

static touch_cal_t s_cal = {0};

const touch_cal_t *touch_get_cal(void)
{
    return &s_cal;
}

void init_touch(void)
{
    /* ----- Initialize the SPI bus for touch ----- */
    spi_bus_config_t buscfg = {
        .sclk_io_num = TOUCH_SPI_SCLK_IO,
        .mosi_io_num = TOUCH_SPI_MOSI_IO,
        .miso_io_num = TOUCH_SPI_MISO_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
        .flags = SPICOMMON_BUSFLAG_MASTER
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* ----- Create "panel io" IO for touch (uses esp_lcd API) ----- */
    esp_lcd_panel_io_spi_config_t tp_io_cfg = {
        .cs_gpio_num = TOUCH_CS_IO,
        .dc_gpio_num = -1,  // not used with touch
        .spi_mode = 0,
        .pclk_hz = TOUCH_SPI_HZ,
        .trans_queue_depth = 3,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_SPI_HOST, &tp_io_cfg, &tp_io));

    /* ----- Configure driver XPT2046 ----- */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = TOUCH_RST_IO,
        .int_gpio_num = TOUCH_IRQ_IO,
        .levels = {
            .reset = 0,        // LOW reset (if RST used)
            .interrupt = 0     // IRQ active LOW on XPT2046
        },
        .flags = {
            .swap_xy = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle));
}

/**
 * @brief Clamp an integer to a [lo, hi] range.
 *
 * @param v  Value to clamp.
 * @param lo Lower bound (inclusive).
 * @param hi Upper bound (inclusive).
 * @return int Clamped value.
 */
static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * @brief Apply current touch calibration to a raw (x,y) reading.
 *
 * If no valid calibration is available, the raw coordinates are clamped to the display bounds.
 * Otherwise, an affine transform is applied:
 * @code
 * x' = xA*x + xB*y + xC
 * y' = yA*x + yB*y + yC
 * @endcode
 *
 * @param raw_x Raw X from controller.
 * @param raw_y Raw Y from controller.
 * @param[out] out_point Output LVGL point (screen space).
 * @param xmax Screen width (max X, exclusive).
 * @param ymax Screen height (max Y, exclusive).
 */
static void apply_touch_calibration(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point, int xmax, int ymax)
{
    if (!s_cal.valid) {
        out_point->x = clampi(raw_x, 0, xmax - 1);
        out_point->y = clampi(raw_y, 0, ymax - 1);
        return;
    }

    float xf = s_cal.xA * raw_x + s_cal.xB * raw_y + s_cal.xC;
    float yf = s_cal.yA * raw_x + s_cal.yB * raw_y + s_cal.yC;

    out_point->x = clampi((int)(xf + 0.5f), 0, xmax - 1);
    out_point->y = clampi((int)(yf + 0.5f), 0, ymax - 1);
}

/**
 * @brief LVGL input device read callback for the touch controller.
 *
 * Reads the latest touch sample from the XPT2046 via esp_lcd_touch, applies calibration,
 * and fills @p data with pointer position and state.
 *
 * @param indev Unused LVGL input device handle.
 * @param data  LVGL input data to fill.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    uint8_t btn = 0;
    bool pressed = false;

    if (touch_handle) {
        if (esp_lcd_touch_read_data(touch_handle) == ESP_OK){
            if (esp_lcd_touch_get_coordinates(touch_handle, &x, &y, NULL, &btn, 1)) {
                pressed = true;
                apply_touch_calibration(x, y, &data->point, TOUCH_X_MAX, TOUCH_Y_MAX);
            }
        }
    }

    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    (void)indev;
}

lv_indev_t *register_touch_with_lvgl(void)
{
    // LVGL v9:
    lv_indev_t *indev = lv_indev_create();                 // create „input device”
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);       // touch/mouse pointer
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);       // callback
    return indev;
}

/**
 * @brief Compute CRC-32 (poly 0xEDB88320, reflected) over a byte buffer.
 *
 * @param data Pointer to data.
 * @param len  Data length in bytes.
 * @return uint32_t CRC-32 of the buffer.
 */
static uint32_t crc32_fast(const void *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t*)data;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return ~crc;
}

esp_err_t touch_cal_save_nvs(const touch_cal_t *cal)
{
    if (!cal || !cal->valid) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TOUCH_CAL_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    touch_cal_t blob = {
        .xA = cal->xA, .xB = cal->xB, .xC = cal->xC,
        .yA = cal->yA, .yB = cal->yB, .yC = cal->yC,
        .magic = 0xC411B007
    };
    blob.crc32 = crc32_fast(&blob, sizeof(blob) - sizeof(blob.crc32));

    err = nvs_set_blob(h, TOUCH_CAL_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool touch_cal_load_nvs(touch_cal_t *existing_cal)
{
    if (!existing_cal) return false;

    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    touch_cal_t blob;
    size_t sz = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, TOUCH_CAL_NVS_KEY, &blob, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(blob)) return false;

    if (blob.magic != 0xC411B007) return false;
    uint32_t crc = crc32_fast(&blob, sizeof(blob) - sizeof(blob.crc32));
    if (crc != blob.crc32) return false;

    s_cal.xA = blob.xA; s_cal.xB = blob.xB; s_cal.xC = blob.xC;
    s_cal.yA = blob.yA; s_cal.yB = blob.yB; s_cal.yC = blob.yC;
    s_cal.valid = true;
    return true;
}

static const int CALIBRATION_MESSAGE_DISPLAY_TIME_MS = 500;

void run_5point_touch_calibration(void)
{
    const char * TAG = "Touch Calibration";

    bsp_display_lock(0);

    /* ----- Create a new screen for calibration ----- */
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *cal_scr = lv_obj_create(NULL);
    lv_screen_load(cal_scr);

    lv_obj_clear_flag(cal_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cal_scr, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(cal_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(cal_scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cal_scr, LV_OPA_COVER, 0);

    ui_show_message("Get Ready For Touch Screen Calibration");

    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(CALIBRATION_MESSAGE_DISPLAY_TIME_MS));
    
    /* ----- Collect raw data ----- */
    for (int i = 0; i < 5; i++) {
        bsp_display_lock(0);
        draw_cross(CAL_POINT_ARROWS[i].tx, CAL_POINT_ARROWS[i].ty);
        bsp_display_unlock();

        sample_raw(&CAL_POINT_ARROWS[i].rx, &CAL_POINT_ARROWS[i].ry);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    /* ----- Calculate the coefficients for the affine transformation ----- */
    float Sx = 0;
    float Sy = 0;
    float Sxx = 0;
    float Syy = 0;
    float Sxy = 0;

    float Sx_tx = 0;
    float Sy_tx = 0;
    float Sx_ty = 0;
    float Sy_ty = 0;
    float S1 = 0;
    float Stx = 0;
    float Sty = 0;

    for (int i = 0; i < 5; i++) {
        float x = CAL_POINT_ARROWS[i].rx;
        float y = CAL_POINT_ARROWS[i].ry;
        float tx = CAL_POINT_ARROWS[i].tx;
        float ty = CAL_POINT_ARROWS[i].ty;

        Sx += x; Sy += y;
        Sxx += x * x;
        Syy += y * y;
        Sxy += x * y;
        S1 += 1.0f;

        Sx_tx += x * tx;
        Sy_tx += y * tx;
        Stx += tx;

        Sx_ty += x * ty;
        Sy_ty += y * ty;
        Sty += ty;
    }

    float denom = (Sxx * Syy - Sxy * Sxy);
    if (fabsf(denom) < 1e-6f) {
        s_cal.valid = false;

        bsp_display_lock(0);
        lv_screen_load(old_scr);
        lv_obj_del(cal_scr);
        bsp_display_unlock();

        ESP_LOGW(TAG, "Calibration failed: singular matrix");
        return;
    }

    // Coefficients for X
    s_cal.xA = (Sx_tx * Syy - Sy_tx * Sxy) / denom;
    s_cal.xB = (Sy_tx * Sxx - Sx_tx * Sxy) / denom;
    s_cal.xC = (Stx - s_cal.xA * Sx - s_cal.xB * Sy) / S1;

    // Coefficients for Y
    s_cal.yA = (Sx_ty * Syy - Sy_ty * Sxy) / denom;
    s_cal.yB = (Sy_ty * Sxx - Sx_ty * Sxy) / denom;
    s_cal.yC = (Sty - s_cal.yA * Sx - s_cal.yB * Sy) / S1;

    s_cal.valid = true;

    /* ----- Get back to the initial screen ----- */
    bsp_display_lock(0);
    lv_screen_load(old_scr);
    lv_obj_del(cal_scr);
    bsp_display_unlock();

    esp_err_t err = touch_cal_save_nvs(&s_cal);
    ESP_LOGI(TAG, "Touch cal saved to NVS: %s", esp_err_to_name(err));    
}

void sample_raw(int *rx, int *ry)
{
    uint32_t sx = 0;
    uint32_t sy = 0;
    uint32_t n = 0;

    while (n < 12) {
        uint16_t x, y;
        uint8_t btn;
        esp_lcd_touch_read_data(touch_handle);

        if (esp_lcd_touch_get_coordinates(touch_handle, &x, &y, NULL, &btn, 1)) {
            sx += x;
            sy += y;
            n++;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    *rx = sx / n;
    *ry = sy / n;
}