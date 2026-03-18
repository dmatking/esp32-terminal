#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "display.h"

#define TERM_COLS CONFIG_TERM_COLS
#define TERM_ROWS CONFIG_TERM_ROWS

typedef struct {
    char     cells[TERM_ROWS][TERM_COLS];
    int      cursor_row;
    int      cursor_col;

    // VT100 parser state
    int      state;         // 0=normal, 1=esc, 2=csi
    char     params[32];
    int      param_len;
} term_t;

void term_init(term_t *t);
void term_feed(term_t *t, const uint8_t *data, int len);
void term_render(const term_t *t, display_t *display);
