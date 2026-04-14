/*
 * bt_kbd_ui.c — thin adapter between esp-ble-kbd-host and esp32-terminal's UI.
 *
 * Owns everything that was display/GPIO coupling in the old bt_kbd.c:
 *   - Wires on_key     → key queue
 *   - Wires on_status  → display_show_status()
 *   - Wires on_passkey → display_show_passkey()
 *   - Wires on_scan_updated → draw_device_list() (moved here from bt_kbd.c)
 *   - boot_button_task: polls GPIO 35, drives cursor + pairing trigger
 */

#include "bt_kbd_ui.h"
#include "display.h"
#include "esp_ble_kbd_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define BOOT_BUTTON_GPIO  35

static const char *TAG = "bt_kbd_ui";

static QueueHandle_t  s_key_queue;
static display_t     *s_display;

// Cursor state for scan UI (owned here; shared between on_scan_updated + boot task)
static volatile int   s_cursor;
static volatile int   s_scan_count;

// ---------------------------------------------------------------------------
// Scan device list rendering (moved from bt_kbd.c)
// ---------------------------------------------------------------------------

static void draw_device_list(int cursor, int count,
                             const ble_kbd_scan_dev_t *devs)
{
    display_t *d = s_display;
    if (!d) return;

    const int scale = 2;
    const int cw    = d->geom.char_w * scale;
    const int ch    = d->geom.char_h * scale;
    const int mx    = d->geom.margin_x;
    const int my    = d->geom.margin_y;
    const int cols  = (d->geom.width  - 2 * mx) / cw;
    const int rows2 = (d->geom.height - 2 * my) / ch;
    const int cols1 = (d->geom.width  - 2 * mx) / d->geom.char_w;
    const rgb_t fg  = {0x00, 0xFF, 0x00};

    char line[64];

    // Header
    snprintf(line, sizeof(line), "%-*.*s", cols, cols, "BT Keyboard Setup");
    display_text_scaled(d, mx, my + 0 * ch, line, scale, fg);

    // Separator
    {
        char sep[32];
        int n = cols < (int)sizeof(sep) - 1 ? cols : (int)sizeof(sep) - 1;
        memset(sep, '-', n);
        sep[n] = '\0';
        snprintf(line, sizeof(line), "%-*.*s", cols, cols, sep);
    }
    display_text_scaled(d, mx, my + 1 * ch, line, scale, fg);

    // Device rows
    for (int r = 2; r < rows2; r++) {
        int i = r - 2;
        if (i < count) {
            char rssi_str[12];
            snprintf(rssi_str, sizeof(rssi_str), "%d dB", devs[i].rssi);
            int rlen   = (int)strlen(rssi_str);
            int name_w = cols - 2 - 1 - rlen;
            snprintf(line, sizeof(line), "%c %-*.*s %s",
                     (i == cursor) ? '>' : ' ',
                     name_w, name_w, devs[i].name, rssi_str);
        } else if (i == 0 && count == 0) {
            snprintf(line, sizeof(line), "%-*.*s", cols, cols, "Scanning...");
        } else {
            snprintf(line, sizeof(line), "%-*.*s", cols, cols, "");
        }
        display_text_scaled(d, mx, my + r * ch, line, scale, fg);
    }

    // Footer
    int fy2 = d->geom.height - my - d->geom.char_h;
    int fy1 = fy2 - d->geom.char_h - 4;
    snprintf(line, sizeof(line), "%-*.*s", cols1, cols1, "Tap device to connect");
    display_text_scaled(d, mx, fy1, line, 1, fg);
    snprintf(line, sizeof(line), "%-*.*s", cols1, cols1,
             "BOOT: next  |  Hold 1s: connect  |  Hold 2s: re-pair");
    display_text_scaled(d, mx, fy2, line, 1, fg);

    display_fb_commit(d);
}

// ---------------------------------------------------------------------------
// esp-ble-kbd-host callbacks
// ---------------------------------------------------------------------------

static void on_key(char ascii, void *ctx)
{
    if (ascii == '\0') return;  // synthetic wakeup — discard
    xQueueSend(s_key_queue, &ascii, 0);
}

static void on_status(ble_kbd_state_t state,
                      const char *line1, const char *line2, void *ctx)
{
    ESP_LOGI(TAG, "kbd state %d: %s / %s", state, line1, line2);
    if (s_display)
        display_show_status(s_display, line1, line2);
}

static void on_passkey(uint32_t passkey, void *ctx)
{
    ESP_LOGI(TAG, "passkey: %06lu", (unsigned long)passkey);
    if (s_display)
        display_show_passkey(s_display, passkey);
}

static void on_scan_updated(const ble_kbd_scan_dev_t *devs,
                            int count, void *ctx)
{
    s_scan_count = count;
    draw_device_list((int)s_cursor, count, devs);
}

// ---------------------------------------------------------------------------
// BOOT button task
// ---------------------------------------------------------------------------

static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask  = 1ULL << BOOT_BUTTON_GPIO,
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&cfg);

    int held_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;

            ble_kbd_state_t state = ble_kbd_host_get_state();

            if (state == BLE_KBD_STATE_SCANNING) {
                if (held_ms >= 1000) {
                    int cnt = s_scan_count;
                    if (cnt > 0) {
                        ESP_LOGI(TAG, "BOOT 1s: selecting device %d", (int)s_cursor);
                        ble_kbd_host_select_device((int)s_cursor);
                        held_ms = 0;
                    }
                }
            } else {
                if (held_ms >= 2000) {
                    ESP_LOGI(TAG, "BOOT 2s: forcing re-pair");
                    ble_kbd_host_start_pairing();
                    held_ms = 0;
                }
            }
        } else {
            if (held_ms > 0 && held_ms < 1000) {
                // Short press: advance cursor during scan
                if (ble_kbd_host_get_state() == BLE_KBD_STATE_SCANNING) {
                    int cnt = s_scan_count;
                    if (cnt > 0) {
                        s_cursor = ((int)s_cursor + 1) % cnt;
                        ESP_LOGD(TAG, "cursor -> %d", (int)s_cursor);
                    }
                }
            }
            held_ms = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

QueueHandle_t bt_kbd_ui_init(display_t *display, bool force_repair)
{
    s_display   = display;
    s_cursor    = 0;
    s_scan_count = 0;
    s_key_queue = xQueueCreate(64, sizeof(char));

    ble_kbd_host_config_t cfg = {
        .force_repair = force_repair,
        .callbacks = {
            .on_key          = on_key,
            .on_status       = on_status,
            .on_passkey      = on_passkey,
            .on_scan_updated = on_scan_updated,
            .ctx             = NULL,
        },
    };
    ESP_ERROR_CHECK(ble_kbd_host_init(&cfg));

    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);

    return s_key_queue;
}

void bt_kbd_ui_wait_connected(void)
{
    ble_kbd_host_wait_connected();
}
