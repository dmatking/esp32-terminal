// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// GT911 touch controller — direct I2C using legacy driver.
// Bypasses esp_lcd_panel_io because v1 (legacy) doesn't handle
// GT911's 16-bit register addressing correctly.
//
// Ported from esp32-p4-webradio; simplified to tap-only detection.

#include "touch.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <string.h>

#define TAG "touch"

#define DISP_W           720
#define DISP_H           720

#define GT911_ADDR       0x5D
#define GT911_ADDR_ALT   0x14
#define I2C_PORT         I2C_NUM_0
#define I2C_SCL          GPIO_NUM_8
#define I2C_SDA          GPIO_NUM_7
#define I2C_CLK_HZ       100000

// GT911 registers (16-bit)
#define GT911_PRODUCT_ID   0x8140
#define GT911_X_RES_L      0x8048
#define GT911_Y_RES_L      0x804A
#define GT911_READ_XY      0x814E
#define GT911_POINT1       0x814F

// Tap detection thresholds
#define TAP_MAX_MOVE     25   // max pixel movement to count as tap (not swipe)
#define RELEASE_THRESHOLD 2   // consecutive no-touch polls before triggering release

static uint8_t  s_addr = 0;  // 0 = not initialized
static uint16_t s_x_res = DISP_W;
static uint16_t s_y_res = DISP_H;

// Gesture state
static bool     s_touching;
static uint16_t s_start_x, s_start_y;
static uint16_t s_cur_x,   s_cur_y;
static int      s_no_touch_count;
static int      s_i2c_errors;

// Pending tap
static bool     s_tap_pending;
static uint16_t s_tap_x, s_tap_y;

// -- I2C primitives -----------------------------------------------------------

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1)
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t gt911_write_byte(uint16_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// -- Public API ---------------------------------------------------------------

esp_err_t touch_init(void)
{
    // Initialize I2C bus
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // Try both GT911 addresses
    const uint8_t addrs[] = { GT911_ADDR, GT911_ADDR_ALT };
    for (int a = 0; a < 2; a++) {
        s_addr = addrs[a];
        uint8_t id[4] = {0};
        if (gt911_read(GT911_PRODUCT_ID, id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 at 0x%02X: %c%c%c%c", addrs[a],
                     id[0], id[1], id[2], id[3]);
            uint8_t res[4] = {0};
            if (gt911_read(GT911_X_RES_L, res, 4) == ESP_OK) {
                s_x_res = res[0] | (res[1] << 8);
                s_y_res = res[2] | (res[3] << 8);
            }
            ESP_LOGI(TAG, "GT911 resolution: %dx%d", s_x_res, s_y_res);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "GT911 at 0x%02X: not found", addrs[a]);
    }
    s_addr = 0;
    ESP_LOGE(TAG, "GT911 not found at either address");
    return ESP_FAIL;
}

bool touch_poll_tap(uint16_t *x, uint16_t *y)
{
    // Return a pending tap immediately
    if (s_tap_pending) {
        s_tap_pending = false;
        *x = s_tap_x;
        *y = s_tap_y;
        return true;
    }

    if (!s_addr) return false;

    // Back off on repeated I2C errors
    if (s_i2c_errors > 5) {
        s_i2c_errors--;
        return false;
    }

    uint8_t status = 0;
    if (gt911_read(GT911_READ_XY, &status, 1) != ESP_OK) {
        s_i2c_errors++;
        return false;
    }
    s_i2c_errors = 0;

    bool buffer_ready = (status & 0x80) != 0;
    uint8_t num_points = status & 0x0F;

    if (buffer_ready)
        gt911_write_byte(GT911_READ_XY, 0);  // clear buffer-ready flag

    if (!buffer_ready)
        return false;

    if (num_points > 0) {
        s_no_touch_count = 0;
        uint8_t pt[8] = {0};
        if (gt911_read(GT911_POINT1, pt, 8) == ESP_OK) {
            uint16_t raw_x = pt[1] | (pt[2] << 8);
            uint16_t raw_y = pt[3] | (pt[4] << 8);
            // Scale from GT911 resolution to display pixels
            uint16_t px = (s_x_res != DISP_W)
                ? (uint16_t)((uint32_t)raw_x * DISP_W / s_x_res) : raw_x;
            uint16_t py = (s_y_res != DISP_H)
                ? (uint16_t)((uint32_t)raw_y * DISP_H / s_y_res) : raw_y;
            if (px >= DISP_W) px = DISP_W - 1;
            if (py >= DISP_H) py = DISP_H - 1;
            // GT911 Y axis is inverted relative to the display
            py = (DISP_H - 1) - py;

            if (!s_touching) {
                s_start_x = px;
                s_start_y = py;
                s_touching = true;
            }
            s_cur_x = px;
            s_cur_y = py;
        }
        return false;
    }

    // buffer_ready, num_points == 0: possible finger-up
    if (!s_touching) return false;

    s_no_touch_count++;
    if (s_no_touch_count < RELEASE_THRESHOLD) return false;

    // Confirmed release
    s_touching = false;
    s_no_touch_count = 0;

    int dx = (int)s_cur_x - (int)s_start_x;
    int dy = (int)s_cur_y - (int)s_start_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx < TAP_MAX_MOVE && ady < TAP_MAX_MOVE) {
        ESP_LOGI(TAG, "Tap at (%d,%d)", s_start_x, s_start_y);
        *x = s_start_x;
        *y = s_start_y;
        return true;
    }
    return false;
}
