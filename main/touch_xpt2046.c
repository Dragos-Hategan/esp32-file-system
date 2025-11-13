#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "touch_xpt2046.h"
#include "calibration_xpt2046.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"

static lv_indev_t *touch_indev = NULL;

lv_indev_t *touch_get_indev(void)
{
    return touch_indev;
}

static esp_lcd_touch_handle_t touch_handle = NULL;

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return touch_handle;
}

/**
 * @brief Register the touch controller as an LVGL pointer device.
 *
 * Creates an LVGL input device, sets it to pointer type, and attaches the
 * @ref lvgl_touch_read_cb callback.
 *
 * @return lv_indev_t* LVGL input device handle.
 */
static lv_indev_t *register_touch_with_lvgl(void);

/**
 * @brief LVGL input device read callback for the touch controller.
 *
 * Reads the latest touch sample from the XPT2046 via esp_lcd_touch, applies calibration,
 * and fills @p data with pointer position and state.
 *
 * @param indev Unused LVGL input device handle.
 * @param data  LVGL input data to fill.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

esp_err_t init_touch(void)
{
    static bool s_inited = false;
    if (s_inited)
        return ESP_OK;

    esp_err_t err;

    /* ----- Initialize the SPI bus for touch ----- */
    static bool spi_bus_inited = true; // The touch driver shares the SPI bus with ILI9341
    if (!spi_bus_inited){
        spi_bus_config_t buscfg = {
            .sclk_io_num = TOUCH_SPI_SCLK_IO,
            .mosi_io_num = TOUCH_SPI_MOSI_IO,
            .miso_io_num = TOUCH_SPI_MISO_IO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 0,
            .flags = SPICOMMON_BUSFLAG_MASTER,
        };
        err = spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
            return err;
    }

    /* ----- Create "panel io" IO for touch (uses esp_lcd API) ----- */
    esp_lcd_panel_io_spi_config_t tp_io_cfg = {
        .cs_gpio_num = TOUCH_CS_IO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = TOUCH_SPI_HZ,
        .trans_queue_depth = 3,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {.lsb_first = 0, .cs_high_active = 0},
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_SPI_HOST, &tp_io_cfg, &tp_io);
    if (err != ESP_OK)
    {
        spi_bus_free(TOUCH_SPI_HOST);
        return err;
    }

    /* ----- Configure driver XPT2046 ----- */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = TOUCH_RST_IO,
        .int_gpio_num = TOUCH_IRQ_IO,
        .levels = {
            .reset = 0,    // LOW reset (if RST used)
            .interrupt = 0 // IRQ active LOW on XPT2046
        },
        .flags = {
            .swap_xy = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };
    err = esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle);
    if (err != ESP_OK)
    {
        esp_lcd_panel_io_del(tp_io);
        spi_bus_free(TOUCH_SPI_HOST);
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

bool register_touch_to_lvgl(void)
{
    bsp_display_lock(0);
    touch_indev = register_touch_with_lvgl();
    if (touch_indev == NULL)
    {
        bsp_display_unlock();
        return false;
    }
    bsp_display_unlock();
    ESP_LOGI("Touch Driver Registration", "XPT2046 touch registered to LVGL");

    return true;
}

static lv_indev_t *register_touch_with_lvgl(void)
{
    // LVGL v9:
    lv_indev_t *indev = lv_indev_create();           // create „input device”
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER); // touch/mouse pointer
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb); // callback
    return indev;
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    uint8_t btn = 0;
    bool pressed = false;

    if (touch_handle)
    {
        if (esp_lcd_touch_read_data(touch_handle) == ESP_OK)
        {
            if (esp_lcd_touch_get_coordinates(touch_handle, &x, &y, NULL, &btn, 1))
            {
                pressed = true;
                apply_touch_calibration(x, y, &data->point, TOUCH_X_MAX, TOUCH_Y_MAX);
            }
        }
    }

    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    (void)indev;
}
