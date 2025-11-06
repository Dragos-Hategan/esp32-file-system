// touch_xpt2046.h
#ifndef TOUCH_XPT2046_H
#define TOUCH_XPT2046_H

#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "driver/spi_master.h"

/* ---------------------- XPT2046 TOUCH CONFIG ---------------------- */
// SPI Config
#define TOUCH_SPI_HOST      SPI3_HOST
#define TOUCH_SPI_SCLK_IO   GPIO_NUM_4
#define TOUCH_CS_IO         GPIO_NUM_5
#define TOUCH_SPI_MOSI_IO   GPIO_NUM_6
#define TOUCH_SPI_MISO_IO   GPIO_NUM_7

// IRQ + RST:
#define TOUCH_IRQ_IO        GPIO_NUM_1      // active LOW on XPT2046
#define TOUCH_RST_IO        -1     

// Panel dimensions and orientation
#define TOUCH_X_MAX         320
#define TOUCH_Y_MAX         240
#define TOUCH_SWAP_XY       true   // for landscape with ILI9341
#define TOUCH_MIRROR_X      true
#define TOUCH_MIRROR_Y      true

#define TOUCH_SPI_HZ        (2 * 1000 * 1000)

#define TOUCH_CAL_NVS_NS     "touch_cal"
#define TOUCH_CAL_NVS_KEY    "affine_v1"
/* ---------------------- XPT2046 TOUCH CONFIG ---------------------- */

typedef struct {
    float xA, xB, xC;   // for x' = xA*x + xB*y + xC
    float yA, yB, yC;   // for y' = yA*x + yB*y + yC
    bool valid;
    uint32_t magic;     // 0xC411B007
    uint32_t crc32;     // simple, for integrity
} touch_cal_t;

typedef struct{ 
    int tx;
    int ty; 
    int rx;
    int ry; 
} cal_point_t;

extern esp_lcd_touch_handle_t s_touch;
extern touch_cal_t s_cal;
extern cal_point_t cal_point_arrows[5];

void init_touch(void);
lv_indev_t *register_touch_with_lvgl(void);
esp_err_t touch_cal_save_nvs(const touch_cal_t *cal);
bool touch_cal_load_nvs(touch_cal_t *out);
/** 
 * @warning This function assumes there is no LVGL display lock already acquired.
 */
void run_touch_calibration_5p(esp_lcd_touch_handle_t s_touch);
void sample_raw(int *rx, int *ry, esp_lcd_touch_handle_t s_touch);

#endif // TOUCH_XPT2046_H