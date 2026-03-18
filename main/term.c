#include "term.h"
#include <string.h>

#define STATE_NORMAL 0
#define STATE_ESC    1
#define STATE_CSI    2

void term_init(term_t *t)
{
    memset(t, 0, sizeof(*t));
}

static void term_scroll(term_t *t)
{
    memmove(t->cells[0], t->cells[1], sizeof(t->cells[0]) * (TERM_ROWS - 1));
    memset(t->cells[TERM_ROWS - 1], ' ', TERM_COLS);
}

static void term_newline(term_t *t)
{
    t->cursor_col = 0;
    t->cursor_row++;
    if (t->cursor_row >= TERM_ROWS) {
        term_scroll(t);
        t->cursor_row = TERM_ROWS - 1;
    }
}

static void term_putc(term_t *t, char c)
{
    if (t->cursor_col >= TERM_COLS)
        term_newline(t);
    t->cells[t->cursor_row][t->cursor_col++] = c;
}

// Parse "row;col" or "val" from CSI params, 1-based, returns 0 if not present
static int csi_param(const char *params, int idx)
{
    int val = 0;
    int cur = 0;
    for (const char *p = params; ; p++) {
        if (*p == ';' || *p == '\0') {
            if (cur == idx) return val;
            val = 0;
            cur++;
            if (*p == '\0') break;
        } else if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        }
    }
    return 0;
}

static void term_dispatch_csi(term_t *t, char cmd)
{
    int p0 = csi_param(t->params, 0);
    int p1 = csi_param(t->params, 1);

    switch (cmd) {
    case 'H': case 'f': {
        // ESC[row;colH -- 1-based
        int row = (p0 > 0 ? p0 : 1) - 1;
        int col = (p1 > 0 ? p1 : 1) - 1;
        t->cursor_row = row < TERM_ROWS ? row : TERM_ROWS - 1;
        t->cursor_col = col < TERM_COLS ? col : TERM_COLS - 1;
        break;
    }
    case 'A': // cursor up
        t->cursor_row -= (p0 > 0 ? p0 : 1);
        if (t->cursor_row < 0) t->cursor_row = 0;
        break;
    case 'B': // cursor down
        t->cursor_row += (p0 > 0 ? p0 : 1);
        if (t->cursor_row >= TERM_ROWS) t->cursor_row = TERM_ROWS - 1;
        break;
    case 'C': // cursor forward
        t->cursor_col += (p0 > 0 ? p0 : 1);
        if (t->cursor_col >= TERM_COLS) t->cursor_col = TERM_COLS - 1;
        break;
    case 'D': // cursor backward
        t->cursor_col -= (p0 > 0 ? p0 : 1);
        if (t->cursor_col < 0) t->cursor_col = 0;
        break;
    case 'J': // erase in display
        if (p0 == 2 || p0 == 3) {
            for (int r = 0; r < TERM_ROWS; r++)
                memset(t->cells[r], ' ', TERM_COLS);
            t->cursor_row = 0;
            t->cursor_col = 0;
        } else if (p0 == 0) {
            // erase from cursor to end
            for (int c = t->cursor_col; c < TERM_COLS; c++)
                t->cells[t->cursor_row][c] = ' ';
            for (int r = t->cursor_row + 1; r < TERM_ROWS; r++)
                memset(t->cells[r], ' ', TERM_COLS);
        }
        break;
    case 'K': // erase in line
        if (p0 == 0)
            for (int c = t->cursor_col; c < TERM_COLS; c++)
                t->cells[t->cursor_row][c] = ' ';
        else if (p0 == 1)
            for (int c = 0; c <= t->cursor_col; c++)
                t->cells[t->cursor_row][c] = ' ';
        else if (p0 == 2)
            memset(t->cells[t->cursor_row], ' ', TERM_COLS);
        break;
    case 'm': // SGR -- ignore color/style attributes
        break;
    default:
        break;
    }
}

void term_feed(term_t *t, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t ch = data[i];

        if (t->state == STATE_ESC) {
            if (ch == '[') {
                t->state = STATE_CSI;
                t->param_len = 0;
                memset(t->params, 0, sizeof(t->params));
            } else {
                t->state = STATE_NORMAL;
            }
            continue;
        }

        if (t->state == STATE_CSI) {
            if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?' || ch == '>') {
                if (t->param_len < (int)sizeof(t->params) - 1)
                    t->params[t->param_len++] = ch;
            } else {
                term_dispatch_csi(t, ch);
                t->state = STATE_NORMAL;
            }
            continue;
        }

        // STATE_NORMAL
        switch (ch) {
        case '\x1b': // ESC
            t->state = STATE_ESC;
            break;
        case '\r':
            t->cursor_col = 0;
            break;
        case '\n':
            term_newline(t);
            break;
        case '\b':
            if (t->cursor_col > 0) t->cursor_col--;
            break;
        case '\t':
            t->cursor_col = (t->cursor_col + 8) & ~7;
            if (t->cursor_col >= TERM_COLS) t->cursor_col = TERM_COLS - 1;
            break;
        default:
            if (ch >= 32 && ch < 127)
                term_putc(t, ch);
            break;
        }
    }
}

void term_render(const term_t *t, display_t *display)
{
    // Write every cell (including spaces) so dirty-row tracking handles
    // both drawing and erasing without a full-screen clear.
    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            char c = t->cells[row][col];
            if (!c) c = ' ';
            display_putc(display, col * display->geom.char_w,
                                  row * display->geom.char_h, c);
        }
    }

    // Underline cursor
    int cx = t->cursor_col * display->geom.char_w;
    int cy = t->cursor_row * display->geom.char_h + display->geom.char_h - 1;
    for (int px = 0; px < display->geom.char_w - 1; px++)
        display_pixel(display, cx + px, cy, true);

    display_flush(display);
}
