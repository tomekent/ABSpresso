// Hardware bring-up for the Waveshare ESP32-S3-Touch-LCD-1.85C.
// Pin assignments and the alternate ST77916 init table come from Waveshare's demo.

#include "board.h"
#include "power.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

// I2C: expander, touch, ES8311, ES7210, RTC
#define PIN_I2C_SDA 11
#define PIN_I2C_SCL 10

// QSPI LCD (ST77916). Reset is on expander pin 1 (EXIO2).
#define LCD_HOST      SPI2_HOST
#define PIN_LCD_SCK   40
#define PIN_LCD_D0    46
#define PIN_LCD_D1    45
#define PIN_LCD_D2    42
#define PIN_LCD_D3    41
#define PIN_LCD_CS    21
#define PIN_LCD_BL    5
#define LCD_PCLK_HZ   (80 * 1000 * 1000)
#define LCD_BUF_LINES 36

// Touch (CST816). Reset is on expander pin 0 (EXIO1).
#define PIN_TOUCH_INT 4

#define EXIO_TOUCH_RST IO_EXPANDER_PIN_NUM_0
#define EXIO_LCD_RST   IO_EXPANDER_PIN_NUM_1

// I2S out. V1 boards have a PCM5101 DAC (no control bus, clock derived from BCK);
// V2 boards have an ES8311 codec on I2C that needs MCLK and an amp enable pin.
#define PIN_I2S_BCLK 48
#define PIN_I2S_WS   38
#define PIN_I2S_DOUT 47
#define PIN_I2S_MCLK 2   // V2 only
#define PIN_PA_EN    15  // V2 only: NS4150B amplifier enable, active high
#define ES8311_ADDR  0x18

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_io_handle_t s_panel_io, s_touch_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static bool s_rotated;
static esp_io_expander_handle_t s_expander;
static i2s_chan_handle_t s_i2s_tx;
static i2c_master_dev_handle_t s_es8311;  // non-NULL on V2 boards
static bool s_i2s_enabled;
static int s_rate, s_channels;
static int s_vol_q15 = 32768 / 2;

// Panels reporting ID 00 02 7F 7F need this table instead of the driver default.
static const st77916_lcd_init_cmd_t lcd_init_alt[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0}, {0xF2, (uint8_t[]){0x28}, 1, 0}, {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0}, {0x83, (uint8_t[]){0xE0}, 1, 0}, {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0}, {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0}, {0xB0, (uint8_t[]){0x56}, 1, 0}, {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0}, {0xB4, (uint8_t[]){0x87}, 1, 0}, {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0}, {0xB7, (uint8_t[]){0x40}, 1, 0}, {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0}, {0xBB, (uint8_t[]){0x08}, 1, 0}, {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0}, {0xC0, (uint8_t[]){0x80}, 1, 0}, {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0}, {0xC3, (uint8_t[]){0x80}, 1, 0}, {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0}, {0xC6, (uint8_t[]){0xA9}, 1, 0}, {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0}, {0xC9, (uint8_t[]){0xA9}, 1, 0}, {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0}, {0xD0, (uint8_t[]){0x91}, 1, 0}, {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0}, {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0}, {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0}, {0xF1, (uint8_t[]){0x10}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0}, {0xF3, (uint8_t[]){0x10}, 1, 0}, {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0}, {0xE2, (uint8_t[]){0x00}, 1, 0}, {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0}, {0xE5, (uint8_t[]){0x06}, 1, 0}, {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0}, {0xE8, (uint8_t[]){0x05}, 1, 0}, {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0}, {0xEB, (uint8_t[]){0x00}, 1, 0}, {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0}, {0xEE, (uint8_t[]){0x00}, 1, 0}, {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0}, {0xF9, (uint8_t[]){0x00}, 1, 0}, {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0}, {0xFC, (uint8_t[]){0x00}, 1, 0}, {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0}, {0xFF, (uint8_t[]){0x00}, 1, 0}, {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0}, {0x62, (uint8_t[]){0x00}, 1, 0}, {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0}, {0x65, (uint8_t[]){0x00}, 1, 0}, {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0}, {0x68, (uint8_t[]){0x00}, 1, 0}, {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0}, {0x6B, (uint8_t[]){0x00}, 1, 0}, {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0}, {0x72, (uint8_t[]){0x00}, 1, 0}, {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0}, {0x75, (uint8_t[]){0x00}, 1, 0}, {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0}, {0x78, (uint8_t[]){0x00}, 1, 0}, {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0}, {0x7B, (uint8_t[]){0x00}, 1, 0}, {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0}, {0x82, (uint8_t[]){0x06}, 1, 0}, {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0}, {0x85, (uint8_t[]){0x04}, 1, 0}, {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0}, {0x88, (uint8_t[]){0x48}, 1, 0}, {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0}, {0x8B, (uint8_t[]){0x02}, 1, 0}, {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0}, {0x8E, (uint8_t[]){0x00}, 1, 0}, {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0}, {0x91, (uint8_t[]){0x00}, 1, 0}, {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0}, {0x94, (uint8_t[]){0xDA}, 1, 0}, {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0}, {0x97, (uint8_t[]){0x00}, 1, 0}, {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0}, {0x9A, (uint8_t[]){0x0C}, 1, 0}, {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0}, {0x9D, (uint8_t[]){0x04}, 1, 0}, {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0}, {0xA0, (uint8_t[]){0x48}, 1, 0}, {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0}, {0xA3, (uint8_t[]){0x02}, 1, 0}, {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0}, {0xA6, (uint8_t[]){0x00}, 1, 0}, {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0}, {0xA9, (uint8_t[]){0x00}, 1, 0}, {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0}, {0xAC, (uint8_t[]){0xD7}, 1, 0}, {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0}, {0xAF, (uint8_t[]){0x00}, 1, 0}, {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0}, {0xB2, (uint8_t[]){0x09}, 1, 0}, {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0}, {0xB5, (uint8_t[]){0x04}, 1, 0}, {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0}, {0xB8, (uint8_t[]){0x48}, 1, 0}, {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0}, {0xBB, (uint8_t[]){0x02}, 1, 0}, {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0}, {0xBE, (uint8_t[]){0x00}, 1, 0}, {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0}, {0xC1, (uint8_t[]){0x47}, 1, 0}, {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0}, {0xC4, (uint8_t[]){0x74}, 1, 0}, {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0}, {0xC7, (uint8_t[]){0x01}, 1, 0}, {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0}, {0xD0, (uint8_t[]){0x10}, 1, 0}, {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0}, {0xD3, (uint8_t[]){0x65}, 1, 0}, {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0}, {0xD6, (uint8_t[]){0x99}, 1, 0}, {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0}, {0xD9, (uint8_t[]){0xAA}, 1, 0}, {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0}, {0x21, (uint8_t[]){0x00}, 1, 0}, {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

static esp_err_t i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(s_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_expander),
                        TAG, "TCA9554 init failed");
    const uint32_t rst_pins = EXIO_TOUCH_RST | EXIO_LCD_RST;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, rst_pins, IO_EXPANDER_OUTPUT), TAG, "set dir");
    // Pulse both resets together.
    esp_io_expander_set_level(s_expander, rst_pins, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(s_expander, rst_pins, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // The vendor demo leaves every EXIO pin as a high output; the audio amp's
    // shutdown line is among them, so do the same.
    esp_io_expander_set_dir(s_expander, 0xFF, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_expander, 0xFF, 1);
    return ESP_OK;
}

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    const ledc_channel_config_t ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&ch);
}

void board_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Reads panel register 0x04 (ID) over a slow link to pick the right init table.
static bool lcd_needs_alt_init(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 3 * 1000 * 1000;
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return false;
    }
    uint8_t id[4] = {0};
    const int cmd = (0x0B << 24) | (0x04 << 8);
    esp_err_t err = esp_lcd_panel_io_rx_param(io, cmd, id, sizeof(id));
    esp_lcd_panel_io_del(io);
    ESP_LOGI(TAG, "LCD ID: %02x %02x %02x %02x (%s)", id[0], id[1], id[2], id[3], esp_err_to_name(err));
    return err == ESP_OK && id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F;
}

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *out_io, esp_lcd_panel_handle_t *out_panel)
{
    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        BOARD_LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    st77916_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    if (lcd_needs_alt_init()) {
        vendor_cfg.init_cmds = lcd_init_alt;
        vendor_cfg.init_cmds_size = sizeof(lcd_init_alt) / sizeof(lcd_init_alt[0]);
    }

    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, out_io), TAG, "panel IO");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(*out_io, &panel_cfg, out_panel), TAG, "panel");
    esp_lcd_panel_reset(*out_panel);
    esp_lcd_panel_init(*out_panel);
    esp_lcd_panel_disp_on_off(*out_panel, true);
    return ESP_OK;
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *out_tp)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &io), TAG, "touch IO");
    s_touch_io = io;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        // No interrupt pin: with one, esp_lvgl_port switches LVGL to event-driven input and only
        // reads the controller when it interrupts. The CST816 doesn't reliably interrupt on
        // release or slow drags, so presses stuck and drags were missed. Polling every refresh
        // period is reliable and cheap.
        .int_gpio_num = GPIO_NUM_NC,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, out_tp), TAG, "touch");

    // The controller powers up with MotionMask = 0, which stops coordinate updates once it has
    // recognised a swipe, so LVGL never sees enough movement to detect a gesture. Enable
    // continuous up/down (bit 1) and left/right (bit 2) tracking.
    uint8_t motion_mask = 0x06, readback = 0;
    esp_lcd_panel_io_tx_param(io, 0xEC, &motion_mask, 1);
    if (esp_lcd_panel_io_rx_param(io, 0xEC, &readback, 1) != ESP_OK || readback != motion_mask) {
        ESP_LOGW(TAG, "couldn't set touch MotionMask (reads %02x); swipes may be unreliable", readback);
    } else {
        ESP_LOGI(TAG, "touch MotionMask set to %02x", readback);
    }
    // Stay awake: asleep, the controller ignores I2C, and we poll it (no interrupt pin).
    uint8_t no_sleep = 0x01;
    esp_lcd_panel_io_tx_param(io, 0xFE, &no_sleep, 1);
    return ESP_OK;
}

static bool touch_poll(lv_point_t *at)
{
    esp_lcd_touch_point_data_t pt;
    uint8_t n = 0;
    if (!s_tp || esp_lcd_touch_read_data(s_tp) != ESP_OK || esp_lcd_touch_get_data(s_tp, &pt, &n, 1) != ESP_OK || !n) {
        return false;
    }
    if (at) {
        // Rotated 180 degrees: the panel mirrors both axes in hardware; touch has to match.
        at->x = s_rotated ? BOARD_LCD_H_RES - 1 - pt.x : pt.x;
        at->y = s_rotated ? BOARD_LCD_V_RES - 1 - pt.y : pt.y;
    }
    return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    lv_point_t at;
    const bool pressed = touch_poll(&at);
    // The power manager sees every touch (to keep the screen awake) and may hold one back.
    if (power_filter_touch(pressed)) {
        data->point = at;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool board_touch_pressed(void)
{
    return touch_poll(NULL);
}

// QSPI command framing for the ST77916: opcode 0x02, command in bits 8..15.
static void lcd_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_panel_io, (0x02 << 24) | (cmd << 8), NULL, 0);
}

void board_display_power(bool on)
{
    if (!s_panel) return;
    if (on) {
        lcd_cmd(0x11);  // sleep out
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_disp_on_off(s_panel, true);
    } else {
        esp_lcd_panel_disp_on_off(s_panel, false);
        lcd_cmd(0x10);  // sleep in
    }
}

void board_set_rotated(bool rotated)
{
    s_rotated = rotated;
    if (s_panel) esp_lcd_panel_mirror(s_panel, rotated, rotated);
}

static esp_err_t es8311_write(uint8_t reg, uint8_t val);

// Codec power-down (Espressif's esp_codec_dev ES8311 suspend sequence): DAC muted, ADC and
// analog off, then clocks off. The next boot runs the full init again.
static void es8311_power_down(void)
{
    static const uint8_t seq[][2] = {
        {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02}, {0x14, 0x00},
        {0x0D, 0xFA}, {0x15, 0x00}, {0x02, 0x10}, {0x00, 0x00}, {0x00, 0x1F},
        {0x01, 0x30}, {0x01, 0x00}, {0x45, 0x00}, {0x0D, 0xFC}, {0x02, 0x00},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        if (es8311_write(seq[i][0], seq[i][1]) != ESP_OK) failed++;
    }
    if (failed) ESP_LOGW(TAG, "ES8311 power-down: %d writes failed", failed);
}

void board_prepare_deep_sleep(void)
{
    board_set_backlight(0);
    board_display_power(false);
    // Let the touch controller drop to its low-power scan; it still pulls INT low on a touch.
    uint8_t auto_sleep = 0x00;
    if (s_touch_io) esp_lcd_panel_io_tx_param(s_touch_io, 0xFE, &auto_sleep, 1);
    if (s_es8311) {
        es8311_power_down();
        // Keep the amplifier off while asleep: a plain output stops being driven in deep sleep.
        gpio_set_level(PIN_PA_EN, 0);
        gpio_hold_en(PIN_PA_EN);
        gpio_deep_sleep_hold_en();
    }
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander");
    backlight_init();

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(lcd_init(&io, &panel), TAG, "LCD");
    s_panel_io = io;
    s_panel = panel;

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 1;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * LCD_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add display");

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) == ESP_OK) {
        // Our own polled input device rather than lvgl_port_add_touch(): that one aborts on any
        // failed I2C read, where a missed read should just count as "not touched".
        lvgl_port_lock(0);
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read);
        s_tp = tp;
        lv_indev_set_display(indev, disp);
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Touch init failed; continuing without touch");
    }

    board_set_backlight(80);
    if (out_disp) {
        *out_disp = disp;
    }
    return ESP_OK;
}

static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8311, buf, sizeof(buf), 100);
}

// Slave mode, MCLK from the MCLK pin at 256 x fs, 16-bit I2S. With a fixed 256 x fs ratio the
// clock dividers are the same at every sample rate (see Espressif's es8311 coefficient table).
static esp_err_t es8311_init(void)
{
    static const uint8_t seq[][2] = {
        {0x00, 0x1F}, {0x00, 0x00}, {0x00, 0x80},  // reset, power on, slave mode
        {0x01, 0x3F},                              // all clocks on, MCLK from pin
        {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},  // pre-div/mult 1, single speed, ADC/DAC OSR
        {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
        {0x09, 0x0C}, {0x0A, 0x0C},                // SDP in/out: I2S, 16-bit
        {0x0D, 0x01}, {0x0E, 0x02},                // power up analog
        {0x12, 0x00}, {0x13, 0x10},                // power up DAC, enable output drive
        {0x1C, 0x6A}, {0x37, 0x08},                // bypass ADC/DAC equalisers
        {0x31, 0x00},                              // unmute
        {0x32, 0xBF},                              // DAC 0 dB; volume is applied in software
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(es8311_write(seq[i][0], seq[i][1]), TAG, "ES8311 reg %02x", seq[i][0]);
        if (i == 0) vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t board_audio_init(void)
{
    const bool v2 = s_i2c_bus && i2c_master_probe(s_i2c_bus, ES8311_ADDR, 50) == ESP_OK;
    ESP_LOGI(TAG, "audio: %s", v2 ? "V2 board (ES8311)" : "V1 board (PCM5101)");
    if (v2) {
        gpio_hold_dis(PIN_PA_EN);  // held low through the last deep sleep, if any
        gpio_deep_sleep_hold_dis();
        const gpio_config_t pa = {.pin_bit_mask = BIT64(PIN_PA_EN), .mode = GPIO_MODE_OUTPUT};
        gpio_config(&pa);
        gpio_set_level(PIN_PA_EN, 0);
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "I2S channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = v2 ? PIN_I2S_MCLK : GPIO_NUM_NC,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = GPIO_NUM_NC,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S std mode");
    s_rate = 44100;
    s_channels = 2;

    if (v2) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8311_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_es8311), TAG, "ES8311 device");
        esp_err_t err = es8311_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 init failed (%s); retrying", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            err = es8311_init();  // the register sequence starts with a reset, so it can simply rerun
        }
        ESP_RETURN_ON_ERROR(err, TAG, "ES8311 init");
    }
    return ESP_OK;
}

esp_err_t board_audio_open(int sample_rate, int channels, int bits)
{
    ESP_RETURN_ON_FALSE(s_i2s_tx && bits == 16 && (channels == 1 || channels == 2), ESP_ERR_INVALID_ARG, TAG,
                        "unsupported format %d/%d/%d", sample_rate, channels, bits);
    if (s_i2s_enabled && sample_rate == s_rate && channels == s_channels) {
        return ESP_OK;
    }
    board_audio_close();
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    const i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx, &clk), TAG, "clock");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s_tx, &slot), TAG, "slot");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "enable");
    s_rate = sample_rate;
    s_channels = channels;
    s_i2s_enabled = true;
    if (s_es8311) gpio_set_level(PIN_PA_EN, 1);
    ESP_LOGI(TAG, "audio out %d Hz x%d", sample_rate, channels);
    return ESP_OK;
}

void board_audio_close(void)
{
    if (s_i2s_enabled) {
        if (s_es8311) gpio_set_level(PIN_PA_EN, 0);
        i2s_channel_disable(s_i2s_tx);
        s_i2s_enabled = false;
    }
}

esp_err_t board_audio_write(int16_t *samples, int len)
{
    if (!s_i2s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    // The DAC has no hardware volume, so scale in place.
    const int n = len / 2;
    for (int i = 0; i < n; i++) {
        samples[i] = (int16_t)((samples[i] * s_vol_q15) >> 15);
    }
    size_t written = 0;
    return i2s_channel_write(s_i2s_tx, samples, len, &written, portMAX_DELAY);
}

void board_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    // Rough perceptual curve: square of the linear control.
    s_vol_q15 = (32768 * volume * volume) / 10000;
}
