#include "menu.h"
#include "ssh_targets.h"
#include "esp_wolfssh_client.h"
#include "term.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
// rtc_io not available on ESP32-P4; deep sleep/wake TBD
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "menu";

// -- Key codes ----------------------------------------------------------------

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_ENTER  '\r'
#define KEY_BS     '\b'
#define KEY_DEL    0x7F
#define KEY_ESC    0x1B

// Read one logical key from the queue, translating VT100 arrow sequences.
// ESC [ A = up, ESC [ B = down
static char read_key(QueueHandle_t keys)
{
    char ch;
    while (1) {
        xQueueReceive(keys, &ch, portMAX_DELAY);
        if (ch != KEY_ESC)
            return ch;

        // Got ESC -- check for [ within 50ms
        char seq;
        if (xQueueReceive(keys, &seq, pdMS_TO_TICKS(50)) != pdTRUE)
            return KEY_ESC;
        if (seq != '[') {
            // Not a CSI sequence, return ESC (discard seq)
            return KEY_ESC;
        }

        // Got ESC [ -- read final char
        if (xQueueReceive(keys, &seq, pdMS_TO_TICKS(50)) != pdTRUE)
            return KEY_ESC;
        switch (seq) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            default:  return KEY_ESC;
        }
    }
}

// -- Display helpers ----------------------------------------------------------

static void draw_list(display_t *disp, ssh_target_t *targets, int count,
                      int cursor)
{
    const int scale = 2;
    const int cw    = disp->geom.char_w * scale;
    const int ch    = disp->geom.char_h * scale;
    const int mx    = disp->geom.margin_x;
    const int my    = disp->geom.margin_y;
    const int cols  = (disp->geom.width  - 2 * mx) / cw;
    const int rows2 = (disp->geom.height - 2 * my) / ch;
    const int cols1 = (disp->geom.width  - 2 * mx) / disp->geom.char_w;
    const rgb_t fg  = {0x00, 0xFF, 0x00};

    char line[128];

    // Header
    snprintf(line, sizeof(line), "%-*.*s", cols, cols, "SSH Targets");
    display_text_scaled(disp, mx, my + 0 * ch, line, scale, fg);

    // Separator
    {
        char sep[32];
        int n = cols < (int)sizeof(sep) - 1 ? cols : (int)sizeof(sep) - 1;
        memset(sep, '-', n);
        sep[n] = '\0';
        snprintf(line, sizeof(line), "%-*.*s", cols, cols, sep);
    }
    display_text_scaled(disp, mx, my + 1 * ch, line, scale, fg);

    // Target rows (rows 2..rows2-3 reserved for list)
    int list_rows = rows2 - 4;  // header(1) + sep(1) + footer(2) = 4
    int top = 0;
    if (count > 0 && cursor >= list_rows)
        top = cursor - list_rows + 1;

    for (int r = 0; r < list_rows; r++) {
        int idx = top + r;
        if (idx < count) {
            snprintf(line, sizeof(line), "%c%-*.*s",
                     idx == cursor ? '>' : ' ',
                     cols - 1, cols - 1,
                     targets[idx].name);
        } else if (r == 0 && count == 0) {
            snprintf(line, sizeof(line), "%-*.*s", cols, cols, "(No targets)");
        } else {
            snprintf(line, sizeof(line), "%-*.*s", cols, cols, "");
        }
        display_text_scaled(disp, mx, my + (2 + r) * ch, line, scale, fg);
    }

    // Footer — 1× scale hint lines
    int fy2 = disp->geom.height - my - disp->geom.char_h;
    int fy1 = fy2 - disp->geom.char_h - 4;
    snprintf(line, sizeof(line), "%-*.*s", cols1, cols1,
             "A: Add  E: Edit  D: Del  Enter: Connect");
    display_text_scaled(disp, mx, fy1, line, 1, fg);
    snprintf(line, sizeof(line), "%-*.*s", cols1, cols1, "Q: Sleep");
    display_text_scaled(disp, mx, fy2, line, 1, fg);

    display_fb_commit(disp);
}

// -- Field editor -------------------------------------------------------------

// Edit a single text field in-place. Returns true if confirmed (Enter),
// false if cancelled (Esc).
static bool edit_field(display_t *disp, QueueHandle_t keys,
                       const char *label, char *buf, int maxlen)
{
    int len = strlen(buf);
    int scroll = 0;
    int cols = disp->geom.cols < 127 ? disp->geom.cols : 127;

    // Initial draw needs a clear
    bool needs_clear = true;
    while (1) {
        if (needs_clear) {
            display_clear(disp);
            needs_clear = false;
        }

        // Pad label and status lines to full width to overwrite old content
        char line[128];
        memset(line, ' ', cols);
        line[cols] = '\0';
        memcpy(line, label, strlen(label));
        display_puts(disp, 0, 0, line);

        // Show the editing buffer with a cursor, padded to full width
        memset(line, ' ', cols);
        line[cols] = '\0';
        int vis_start = scroll;
        for (int i = 0; i < cols && (vis_start + i) < len; i++)
            line[i] = buf[vis_start + i];
        // Place cursor
        int cpos = len - scroll;
        if (cpos >= 0 && cpos < cols)
            line[cpos] = '_';
        display_puts(disp, 0, 2 * disp->geom.char_h, line);

        memset(line, ' ', cols);
        line[cols] = '\0';
        memcpy(line, "enter:ok esc:cancel", 19);
        display_puts(disp, 0, 5 * disp->geom.char_h, line);
        display_flush(disp);

        char ch = read_key(keys);
        if (ch == KEY_ENTER) {
            return true;
        } else if (ch == KEY_ESC) {
            return false;
        } else if (ch == KEY_BS || ch == KEY_DEL) {
            if (len > 0) {
                buf[--len] = '\0';
                if (scroll > 0 && len < scroll + cols)
                    scroll--;
            }
        } else if (ch >= ' ' && ch < 0x7F) {
            if (len < maxlen - 1) {
                buf[len++] = ch;
                buf[len] = '\0';
                if (len - scroll >= cols)
                    scroll++;
            }
        }
    }
}

// -- Edit target (all fields) -------------------------------------------------

// Edit/create a target. Returns true if saved, false if cancelled.
static bool edit_target(display_t *disp, QueueHandle_t keys,
                        ssh_target_t *target, bool is_new)
{
    // Work on a copy so cancel doesn't corrupt the original
    ssh_target_t tmp = *target;

    if (!edit_field(disp, keys, "Name:", tmp.name, SSH_TARGET_NAME_LEN))
        return false;
    if (!edit_field(disp, keys, "Host:", tmp.host, SSH_TARGET_HOST_LEN))
        return false;

    // Port as string
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", tmp.port ? tmp.port : 22);
    if (!edit_field(disp, keys, "Port:", port_str, sizeof(port_str)))
        return false;
    int p = atoi(port_str);
    tmp.port = (p > 0 && p < 65536) ? (uint16_t)p : 22;

    if (!edit_field(disp, keys, "User:", tmp.user, SSH_TARGET_USER_LEN))
        return false;
    if (!edit_field(disp, keys, "Password:", tmp.pass, SSH_TARGET_PASS_LEN))
        return false;

    *target = tmp;
    return true;
}

// -- Delete confirmation ------------------------------------------------------

static bool confirm_delete(display_t *disp, QueueHandle_t keys,
                           const char *name)
{
    display_clear(disp);
    display_puts(disp, 0, 0,                       "Delete target?");
    display_puts(disp, 0, 2 * disp->geom.char_h,   name);
    display_puts(disp, 0, 5 * disp->geom.char_h,   "y:yes n:no");
    display_flush(disp);

    while (1) {
        char ch = read_key(keys);
        if (ch == 'y' || ch == 'Y')
            return true;
        if (ch == 'n' || ch == 'N' || ch == KEY_ESC)
            return false;
    }
}

// -- SSH session wrapper ------------------------------------------------------

// Embedded SSH key pair (compiled in from keys/id_ed25519*)
extern const uint8_t ssh_key_start[]    asm("_binary_id_ed25519_start");
extern const uint8_t ssh_key_end[]      asm("_binary_id_ed25519_end");
extern const uint8_t ssh_pubkey_start[] asm("_binary_id_ed25519_pub_start");
extern const uint8_t ssh_pubkey_end[]   asm("_binary_id_ed25519_pub_end");

typedef struct {
    display_t        *display;
    term_t            term;
    SemaphoreHandle_t done;
} session_ctx_t;

static void session_on_data(const uint8_t *data, size_t len, void *ctx)
{
    session_ctx_t *s = (session_ctx_t *)ctx;
    term_feed(&s->term, data, (int)len);
    term_render(&s->term, s->display);
}

static void session_on_disconnected(int reason, void *ctx)
{
    (void)reason;
    session_ctx_t *s = (session_ctx_t *)ctx;
    display_show_status(s->display, "Disconnected", "");
    xSemaphoreGive(s->done);
}

// Blocking: connects, forwards keys, returns on disconnect.
static void run_ssh_session(display_t *display, QueueHandle_t keys,
                             const ssh_target_t *target)
{
    static session_ctx_t ctx;
    ctx.display = display;
    ctx.done = xSemaphoreCreateBinary();
    term_init(&ctx.term);

    ssh_client_config_t cfg = {
        .host            = target->host,
        .port            = target->port,
        .user            = target->user,
        .privkey_pem     = ssh_key_start,
        .privkey_pem_len = (size_t)(ssh_key_end - ssh_key_start),
        .pubkey_pem      = ssh_pubkey_start,
        .pubkey_pem_len  = (size_t)(ssh_pubkey_end - ssh_pubkey_start),
        .password        = target->pass[0] ? target->pass : NULL,
        .term_cols       = (uint16_t)display->geom.cols,
        .term_rows       = (uint16_t)display->geom.rows,
        .callbacks = {
            .on_data         = session_on_data,
            .on_disconnected = session_on_disconnected,
            .ctx             = &ctx,
        },
    };
    ssh_client_connect(&cfg);

    // Forward keystrokes from the BLE keyboard queue to the SSH session.
    char ch;
    while (xSemaphoreTake(ctx.done, 0) != pdTRUE) {
        if (xQueueReceive(keys, &ch, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint8_t b = (uint8_t)ch;
            ssh_client_send(&b, 1);
        }
    }

    vSemaphoreDelete(ctx.done);
    ctx.done = NULL;
}

// -- Main menu loop -----------------------------------------------------------

void menu_run(display_t *display, QueueHandle_t keys)
{
    ssh_target_t targets[SSH_TARGET_MAX];
    int count = ssh_targets_load(targets, SSH_TARGET_MAX);
    int cursor = 0;

    while (1) {
        if (cursor >= count)
            cursor = count > 0 ? count - 1 : 0;

        draw_list(display, targets, count, cursor);

        char ch = read_key(keys);
        switch (ch) {
        case KEY_UP:
            if (cursor > 0) cursor--;
            break;

        case KEY_DOWN:
            if (cursor < count - 1) cursor++;
            break;

        case KEY_ENTER:
            if (count > 0) {
                ESP_LOGI(TAG, "Connecting to %s (%s:%d)",
                         targets[cursor].name,
                         targets[cursor].host,
                         targets[cursor].port);
                run_ssh_session(display, keys, &targets[cursor]);
                // Returns here after disconnect
                ESP_LOGI(TAG, "Disconnected, back to menu");
            }
            break;

        case 'a':
        case 'A':
            if (count < SSH_TARGET_MAX) {
                ssh_target_t new_target = { .port = 22 };
                if (edit_target(display, keys, &new_target, true)) {
                    if (ssh_target_save(count, &new_target)) {
                        targets[count] = new_target;
                        cursor = count;
                        count++;
                    }
                }
            }
            break;

        case 'e':
        case 'E':
            if (count > 0) {
                ssh_target_t tmp = targets[cursor];
                if (edit_target(display, keys, &tmp, false)) {
                    if (ssh_target_save(cursor, &tmp)) {
                        targets[cursor] = tmp;
                    }
                }
            }
            break;

        case 'd':
        case 'D':
            if (count > 0 &&
                confirm_delete(display, keys, targets[cursor].name)) {
                if (ssh_target_delete(cursor, count)) {
                    // Reload
                    count = ssh_targets_load(targets, SSH_TARGET_MAX);
                }
            }
            break;

        case 'q':
        case 'Q': {
            display_clear(display);
            display_puts(display, 0, 0,                          "Deep sleep?");
            display_puts(display, 0, 2 * display->geom.char_h,  "Press button to");
            display_puts(display, 0, 3 * display->geom.char_h,  "wake up.");
            display_puts(display, 0, 5 * display->geom.char_h,  "y:sleep  n:cancel");
            display_flush(display);
            char confirm = read_key(keys);
            if (confirm == 'y' || confirm == 'Y') {
                display_clear(display);
                display_puts(display, 0, 0, "sleeping...");
                display_flush(display);
                vTaskDelay(pdMS_TO_TICKS(500));
                // TODO: P4 deep sleep + MIPI-DSI power down
                // For now just restart
                esp_restart();
            }
            break;
        }

        default:
            break;
        }
    }
}
