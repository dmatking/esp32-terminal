#include "display.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "mipi_dsi";

// Panel timing for EK79007 1024x600 @ 60Hz (matches factory BSP)
#define DSI_H_RES       1024
#define DSI_V_RES       600
#define DSI_HSYNC       10
#define DSI_HBP         160
#define DSI_HFP         160
#define DSI_VSYNC       1
#define DSI_VBP         23
#define DSI_VFP         12
#define DSI_DPI_CLK_MHZ 52
#define DSI_LANE_NUM    2
#define DSI_LANE_MBPS   1000

// LDO for MIPI PHY (VDD_MIPI_DPHY = 2.5V on LDO_VO3)
#define DSI_PHY_LDO_CHAN    3
#define DSI_PHY_LDO_MV     2500

// Board GPIOs
#define DSI_BK_LIGHT_GPIO   26
#define DSI_RST_GPIO        27

// Inset from display edges (bezel clearance)
#define MARGIN_X  10
#define MARGIN_Y  10

// Character cell: 2x scaled 5x7 font = 10x14, plus 2px gap = 12x16
#define CHAR_W  12
#define CHAR_H  16
#define COLS    ((DSI_H_RES - 2 * MARGIN_X) / CHAR_W)   // 83
#define ROWS    ((DSI_V_RES - 2 * MARGIN_Y) / CHAR_H)   // 36

// RGB888 bytes per pixel
#define BPP     3

// Phosphor green in RGB888
#define FG_R  0x00
#define FG_G  0xFF
#define FG_B  0x00

#define FB_SIZE (DSI_H_RES * DSI_V_RES * BPP)

// Render one character into the framebuffer at pixel position (px, py)
static void render_char(uint8_t *fb, int px, int py, char c)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font5x7[c - 32];

    // Clear the entire cell first
    for (int y = 0; y < CHAR_H; y++) {
        uint8_t *row = fb + ((py + y) * DSI_H_RES + px) * BPP;
        memset(row, 0, CHAR_W * BPP);
    }

    if (c == ' ') return;

    // Draw 2x scaled glyph
    for (int fx = 0; fx < 5; fx++) {
        uint8_t col_data = glyph[fx];
        for (int fy = 0; fy < 7; fy++) {
            if (col_data & (1 << fy)) {
                // 2x2 pixel block
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int x = px + fx * 2 + dx;
                        int y = py + fy * 2 + dy;
                        uint8_t *p = fb + (y * DSI_H_RES + x) * BPP;
                        p[0] = FG_B;
                        p[1] = FG_G;
                        p[2] = FG_R;
                    }
                }
            }
        }
    }
}

// -- HAL ops ------------------------------------------------------------------

static void dsi_clear(display_t *d)
{
    mipi_dsi_priv_t *priv = d->priv;
    memset(priv->cells, ' ', sizeof(priv->cells));
    memset(priv->dirty, 0, sizeof(priv->dirty));
    priv->cleared = true;
}

static esp_err_t dsi_flush(display_t *d)
{
    mipi_dsi_priv_t *priv = d->priv;

    if (priv->cleared) {
        // Don't memset the live framebuffer (causes flicker).
        // Instead mark all rows dirty so render_char clears each cell.
        priv->cleared = false;
        for (int i = 0; i < (ROWS + 63) / 64; i++)
            priv->dirty[i] = ~0ULL;
    }

    // Render dirty rows
    for (int row = 0; row < ROWS; row++) {
        int word = row / 64;
        uint64_t bit = 1ULL << (row % 64);
        if (!(priv->dirty[word] & bit)) continue;

        int py = MARGIN_Y + row * CHAR_H;
        for (int col = 0; col < COLS; col++)
            render_char(priv->framebuf, MARGIN_X + col * CHAR_W, py, priv->cells[row][col]);

        priv->dirty[word] &= ~bit;
    }

    // Write-back cache so hardware sees updated pixels
    esp_cache_msync(priv->framebuf, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // For DPI panels, draw_bitmap with the panel's own buffer just does
    // a cache writeback (no copy). The hardware continuously scans from it.
    esp_lcd_panel_draw_bitmap(priv->panel, 0, 0, DSI_H_RES, DSI_V_RES,
                              priv->framebuf);
    return ESP_OK;
}

static void dsi_pixel(display_t *d, int x, int y, bool on)
{
    mipi_dsi_priv_t *priv = d->priv;
    if (x < 0 || x >= DSI_H_RES || y < 0 || y >= DSI_V_RES) return;
    uint8_t *p = priv->framebuf + (y * DSI_H_RES + x) * BPP;
    if (on) {
        p[0] = FG_B; p[1] = FG_G; p[2] = FG_R;
    } else {
        p[0] = 0; p[1] = 0; p[2] = 0;
    }
    int row = (y - MARGIN_Y) / CHAR_H;
    if (row >= 0 && row < ROWS) {
        priv->dirty[row / 64] |= 1ULL << (row % 64);
    }
}

static int dsi_putc(display_t *d, int x, int y, char c)
{
    mipi_dsi_priv_t *priv = d->priv;
    int col = x / CHAR_W;
    int row = y / CHAR_H;
    if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
        priv->cells[row][col] = c;
        priv->dirty[row / 64] |= 1ULL << (row % 64);
    }
    return CHAR_W;
}

static void dsi_puts(display_t *d, int x, int y, const char *s)
{
    while (*s)
        x += dsi_putc(d, x, y, *s++);
}

static const display_ops_t mipi_dsi_ops = {
    dsi_clear, dsi_flush, dsi_pixel, dsi_putc, dsi_puts
};

// -- Public init --------------------------------------------------------------

esp_err_t display_mipi_dsi_init(display_t *d, mipi_dsi_priv_t *priv)
{
    memset(priv, 0, sizeof(*priv));

    // 1. Power on MIPI DSI PHY via internal LDO
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered on");

    // 2. Create DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    // 3. Create DBI (command) IO
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    // 4. Create DPI (data) panel
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .video_timing = {
            .h_size = DSI_H_RES,
            .v_size = DSI_V_RES,
            .hsync_back_porch = DSI_HBP,
            .hsync_pulse_width = DSI_HSYNC,
            .hsync_front_porch = DSI_HFP,
            .vsync_back_porch = DSI_VBP,
            .vsync_pulse_width = DSI_VSYNC,
            .vsync_front_porch = DSI_VFP,
        },
    };

    // 5. Init EK79007 panel driver
    esp_lcd_panel_handle_t panel = NULL;
    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = DSI_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &dev_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // Display On (0x29) via DBI command interface
    esp_lcd_panel_io_tx_param(dbi_io, 0x29, NULL, 0);
    ESP_LOGI(TAG, "Panel reset and initialized");

    // 6. Turn on backlight
    gpio_config_t bk_gpio_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DSI_BK_LIGHT_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_cfg));
    gpio_set_level(DSI_BK_LIGHT_GPIO, 1);
    ESP_LOGI(TAG, "Backlight on (GPIO %d)", DSI_BK_LIGHT_GPIO);

    // 7. Get the panel's own framebuffer (allocated internally by DPI driver)
    //    Writing directly into this buffer avoids a memcpy - hardware scans from it.
    void *fb0 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0));
    priv->framebuf = (uint8_t *)fb0;
    ESP_LOGI(TAG, "Using panel framebuffer at %p (%d bytes)", fb0, FB_SIZE);

    priv->panel = panel;

    d->ops  = &mipi_dsi_ops;
    d->priv = priv;
    d->geom = (display_geom_t){
        DSI_H_RES, DSI_V_RES, COLS, ROWS, CHAR_W, CHAR_H, MARGIN_X, MARGIN_Y
    };

    // Clear screen
    dsi_clear(d);
    dsi_flush(d);

    ESP_LOGI(TAG, "MIPI-DSI 1024x600 init done (%dx%d terminal)", COLS, ROWS);
    return ESP_OK;
}
