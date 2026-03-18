#include "term.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// -- libvterm screen callbacks ------------------------------------------------

static int on_damage(VTermRect rect, void *user)
{
    term_t *t = (term_t *)user;
    for (int r = rect.start_row; r < rect.end_row && r < TERM_ROWS; r++)
        t->dirty_rows |= (1u << r);
    return 0;
}

static int on_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    term_t *t = (term_t *)user;
    t->cursor_moved = true;
    // Mark both old and new cursor rows dirty
    if (oldpos.row < TERM_ROWS) t->dirty_rows |= (1u << oldpos.row);
    if (pos.row < TERM_ROWS)    t->dirty_rows |= (1u << pos.row);
    return 0;
}

static const VTermScreenCallbacks screen_cbs = {
    .damage     = on_damage,
    .movecursor = on_movecursor,
};

// -- Public API ---------------------------------------------------------------

void term_init(term_t *t)
{
    memset(t, 0, sizeof(*t));
    t->vt = vterm_new(TERM_ROWS, TERM_COLS);
    vterm_set_utf8(t->vt, 1);

    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &screen_cbs, t);
    vterm_screen_reset(t->screen, 1);

    // Mark everything dirty for initial render
    t->dirty_rows = (TERM_ROWS >= 32) ? 0xFFFFFFFF : ((1u << TERM_ROWS) - 1);
}

void term_feed(term_t *t, const uint8_t *data, int len)
{
    vterm_input_write(t->vt, (const char *)data, len);
}

void term_render(term_t *t, display_t *display)
{
    int mx = display->geom.margin_x;
    int my = display->geom.margin_y;
    uint32_t dirty = t->dirty_rows;

    if (!dirty && !t->cursor_moved)
        return;  // nothing changed

    // Render only dirty rows
    for (int row = 0; row < TERM_ROWS; row++) {
        if (!(dirty & (1u << row)))
            continue;

        for (int col = 0; col < TERM_COLS; col++) {
            VTermScreenCell cell;
            VTermPos pos = { .row = row, .col = col };
            vterm_screen_get_cell(t->screen, pos, &cell);

            char c = (cell.chars[0] >= 32 && cell.chars[0] < 127)
                   ? (char)cell.chars[0] : ' ';

            // Convert libvterm colors to RGB
            VTermColor vfg = cell.fg, vbg = cell.bg;
            vterm_state_convert_color_to_rgb(vterm_obtain_state(t->vt), &vfg);
            vterm_state_convert_color_to_rgb(vterm_obtain_state(t->vt), &vbg);
            rgb_t fg = { vfg.rgb.red, vfg.rgb.green, vfg.rgb.blue };
            rgb_t bg = { vbg.rgb.red, vbg.rgb.green, vbg.rgb.blue };

            display_putc_color(display, col * display->geom.char_w,
                                        row * display->geom.char_h, c, fg, bg);
        }

        // Let IDLE task feed the watchdog during heavy rendering
        if ((row & 3) == 3)
            vTaskDelay(1);
    }

    t->dirty_rows = 0;
    t->cursor_moved = false;

    // Draw underline cursor
    VTermState *state = vterm_obtain_state(t->vt);
    VTermPos cursor;
    vterm_state_get_cursorpos(state, &cursor);

    // Erase old cursor if it moved
    if (cursor.row != t->prev_cursor_row || cursor.col != t->prev_cursor_col) {
        int ox = mx + t->prev_cursor_col * display->geom.char_w;
        int oy = my + t->prev_cursor_row * display->geom.char_h + display->geom.char_h - 1;
        for (int px = 0; px < display->geom.char_w - 1; px++)
            display_pixel(display, ox + px, oy, false);
    }

    int cx = mx + cursor.col * display->geom.char_w;
    int cy = my + cursor.row * display->geom.char_h + display->geom.char_h - 1;
    for (int px = 0; px < display->geom.char_w - 1; px++)
        display_pixel(display, cx + px, cy, true);

    t->prev_cursor_row = cursor.row;
    t->prev_cursor_col = cursor.col;

    display_flush(display);
}
