#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "display.h"
#include <vterm.h>

#define TERM_COLS CONFIG_TERM_COLS
#define TERM_ROWS CONFIG_TERM_ROWS

typedef struct {
    VTerm       *vt;
    VTermScreen *screen;
    uint32_t     dirty_rows;   // bitmask of rows needing re-render (up to 32 rows)
    bool         cursor_moved;
    int          prev_cursor_row;
    int          prev_cursor_col;
} term_t;

void term_init(term_t *t);
void term_feed(term_t *t, const uint8_t *data, int len);
void term_render(term_t *t, display_t *display);
