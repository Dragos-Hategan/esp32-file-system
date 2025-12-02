#include "touch_xpt2046.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

#include "esp_lcd_touch_xpt2046.h"
#include "calibration_xpt2046.h"
#include "settings.h"

const char* TAG_TOUCH = "touch_driver";
static esp_lcd_touch_handle_t touch_handle = NULL;
static lv_indev_t *touch_indev = NULL;

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
    esp_err_t touch_init_err = ESP_OK;

    ESP_LOGI(TAG_TOUCH, "Initializing SPI bus");
    bool shared_bus = (CONFIG_TOUCH_SPI_HOST == BSP_LCD_SPI_NUM || CONFIG_TOUCH_SPI_HOST == CONFIG_SDSPI_BUS_HOST);
    if (!shared_bus){
        spi_bus_config_t buscfg = {
            .sclk_io_num = CONFIG_TOUCH_SPI_SCLK_GPIO,
            .mosi_io_num = CONFIG_TOUCH_SPI_MOSI_GPIO,
            .miso_io_num = CONFIG_TOUCH_SPI_MISO_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 0,
            .flags = SPICOMMON_BUSFLAG_MASTER,
        };
        touch_init_err = spi_bus_initialize(CONFIG_TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (touch_init_err != ESP_OK && touch_init_err != ESP_ERR_INVALID_STATE){
            ESP_LOGE(TAG_TOUCH, "Failed to initialize SPI bus: (%s)", esp_err_to_name(touch_init_err));
            return touch_init_err;
        }
    }else{
        ESP_LOGI(TAG_TOUCH, "SPI bus already initialized by another driver");
    }

    ESP_LOGI(TAG_TOUCH, "Create IO panel (uses esp_lcd API)");
    esp_lcd_panel_io_spi_config_t tp_io_cfg = {
        .cs_gpio_num = CONFIG_TOUCH_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = CONFIG_TOUCH_SPI_HZ,
        .trans_queue_depth = 3,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {.lsb_first = 0, .cs_high_active = 0},
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    touch_init_err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_TOUCH_SPI_HOST, &tp_io_cfg, &tp_io);
    if (touch_init_err != ESP_OK)
    {
        if (!shared_bus){
            spi_bus_free(CONFIG_TOUCH_SPI_HOST);
        }
        ESP_LOGE(TAG_TOUCH, "Failed to create panel: (%s)", esp_err_to_name(touch_init_err));
        return touch_init_err;
    }

    ESP_LOGI(TAG_TOUCH, "Configure driver XPT2046");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = CONFIG_TOUCH_RST_GPIO,
        .int_gpio_num = CONFIG_TOUCH_IRQ_GPIO,
        .levels = {
            .reset = 0,    // LOW reset (if RST used)
            .interrupt = 0 // IRQ active LOW on XPT2046
        },
        .flags = {
#ifdef CONFIG_TOUCH_SWAP_XY            
            .swap_xy =  1 ,
#else
            .swap_xy =  0,
#endif

#ifdef CONFIG_TOUCH_MIRROR_X            
            .mirror_x =  1 ,
#else
            .mirror_x =  0,
#endif

#ifdef CONFIG_TOUCH_MIRROR_Y            
            .mirror_y =  1 ,
#else
            .mirror_y =  0,
#endif
        },
    };
    touch_init_err = esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle);
    if (touch_init_err != ESP_OK)
    {
        if (!shared_bus){
            spi_bus_free(CONFIG_TOUCH_SPI_HOST);
        }
        esp_lcd_panel_io_del(tp_io);
        ESP_LOGE(TAG_TOUCH, "Failed to configure driver XPT2046: (%s)", esp_err_to_name(touch_init_err));
        return touch_init_err;
    }

    return ESP_OK;
}

esp_err_t register_touch_to_lvgl(void)
{
    bsp_display_lock(0);
    touch_indev = register_touch_with_lvgl();
    if (touch_indev == NULL)
    {
        bsp_display_unlock();
        ESP_LOGE("touch_driver_registration", "XPT2046 FAILED");
        return ESP_FAIL;
    }
    bsp_display_unlock();
    ESP_LOGI("touch_driver_registration", "XPT2046 touch registered to LVGL");

    return ESP_OK;
}

lv_indev_t *touch_get_indev(void)
{
    return touch_indev;
}

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return touch_handle;
}

void touch_log_press(uint16_t x, uint16_t y)
{
    ESP_LOGI(TAG_TOUCH, "Touch press: x=%u y=%u", (unsigned)x, (unsigned)y);
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
    static bool prev_pressed = false;

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
    if (pressed && !prev_pressed) {
        touch_log_press(x, y);
        settings_start_screensaver_timers();
    }
    prev_pressed = pressed;
    (void)indev;
}
