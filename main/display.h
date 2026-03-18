#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int width, height;   // pixels
    int cols, rows;      // character grid
    int char_w, char_h;  // pixels per character cell
} display_geom_t;

typedef struct display_t display_t;

typedef struct {
    void      (*clear)(display_t *d);
    esp_err_t (*flush)(display_t *d);
    void      (*pixel)(display_t *d, int x, int y, bool on);
    int       (*putc )(display_t *d, int x, int y, char c);
    void      (*puts )(display_t *d, int x, int y, const char *s);
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

// MIPI-DSI backend for ESP32-P4 Function EV Board (1024x600 EK79007)
typedef struct {
    void    *panel;                                        // esp_lcd_panel_handle_t
    uint8_t *framebuf;                                     // RGB888 framebuffer in PSRAM
    char     cells[CONFIG_TERM_ROWS][CONFIG_TERM_COLS];    // character grid
    uint64_t dirty[(CONFIG_TERM_ROWS + 63) / 64];          // dirty row bitmask
    bool     cleared;
} mipi_dsi_priv_t;

esp_err_t display_mipi_dsi_init(display_t *d, mipi_dsi_priv_t *priv);
