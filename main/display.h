#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct {
    char    ch;
    rgb_t   fg;
    rgb_t   bg;
} display_cell_t;

typedef struct {
    int width, height;   // pixels
    int cols, rows;      // character grid
    int char_w, char_h;  // pixels per character cell
    int margin_x, margin_y; // pixel inset from display edges
} display_geom_t;

typedef struct display_t display_t;

typedef struct {
    void      (*clear)(display_t *d);
    esp_err_t (*flush)(display_t *d);
    void      (*pixel)(display_t *d, int x, int y, bool on);
    int       (*putc )(display_t *d, int x, int y, char c);
    void      (*puts )(display_t *d, int x, int y, const char *s);
    int       (*putc_color)(display_t *d, int x, int y, char c, rgb_t fg, rgb_t bg);
} display_ops_t;

struct display_t {
    const display_ops_t *ops;
    display_geom_t       geom;
    void                *priv;
};

static inline void      display_clear(display_t *d)                              { d->ops->clear(d); }
static inline esp_err_t display_flush(display_t *d)                              { return d->ops->flush(d); }
static inline void      display_pixel(display_t *d, int x, int y, bool on)       { d->ops->pixel(d, x, y, on); }
static inline int       display_putc (display_t *d, int x, int y, char c)        { return d->ops->putc(d, x, y, c); }
static inline void      display_puts (display_t *d, int x, int y, const char *s) { d->ops->puts(d, x, y, s); }
static inline int       display_putc_color(display_t *d, int x, int y, char c, rgb_t fg, rgb_t bg)
                                                                                  { return d->ops->putc_color(d, x, y, c, fg, bg); }

// MIPI-DSI backend for ESP32-P4 Function EV Board (1024x600 EK79007)
typedef struct {
    void          *panel;                                        // esp_lcd_panel_handle_t
    uint8_t       *framebuf;                                     // RGB888 framebuffer in PSRAM
    display_cell_t cells[CONFIG_TERM_ROWS][CONFIG_TERM_COLS];    // character + color grid
    uint64_t       dirty[(CONFIG_TERM_ROWS + 63) / 64];          // dirty row bitmask
    bool           cleared;
} mipi_dsi_priv_t;

esp_err_t display_mipi_dsi_init(display_t *d, mipi_dsi_priv_t *priv);
void display_show_splash(display_t *d);
void display_show_status(display_t *d, const char *line1, const char *line2);
void display_show_passkey(display_t *d, uint32_t key);

// Raw framebuffer helpers — bypass the cell grid, useful for scaled layouts.
void display_fb_clear(display_t *d);
void display_text_scaled(display_t *d, int px, int py, const char *s, int scale, rgb_t fg);
void display_fb_commit(display_t *d);
