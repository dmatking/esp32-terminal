#include "bt_kbd.h"
#include "display.h"
#include "touch.h"
#include "freertos/semphr.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_hidh_nimble.h"
#include "esp_log.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "esp_private/esp_hidh_private.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

#define BOOT_BUTTON_GPIO  35  // ESP32-P4 strapping pin (low = download mode at reset)

// No public header for ble_store_config_init -- declared as extern per NimBLE convention
extern void ble_store_config_init(void);
// Lightweight reconnect (our addition to nimble_hidh.c)
extern esp_hidh_dev_t *esp_ble_hidh_dev_reconnect(esp_hidh_dev_t *dev);

static const char *TAG = "bt_kbd";

static QueueHandle_t     s_key_queue;
static display_t        *s_display;
static SemaphoreHandle_t s_connected_sem;
static SemaphoreHandle_t s_disconnected_sem;
static SemaphoreHandle_t s_open_done_sem;
static SemaphoreHandle_t s_scan_mutex;
static bool              s_reconnecting;
static bool              s_force_repair;
static esp_hidh_dev_t   *s_hidh_dev;

// -- Passkey display ----------------------------------------------------------

static void show_passkey(uint32_t key)
{
    if (!s_display) return;
    display_show_passkey(s_display, key);
}

// -- HID keycode -> ASCII -----------------------------------------------------

static const char keymap_normal[] = {
    0,    0,    0,    0,
    'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',
    'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',
    'q',  'r',  's',  't',
    'u',  'v',  'w',  'x',
    'y',  'z',
    '1',  '2',  '3',  '4',  '5',
    '6',  '7',  '8',  '9',  '0',
    '\r', '\x1b','\x7f','\t',' ',
    '-',  '=',  '[',  ']',  '\\',
    0,    ';',  '\'', '`',
    ',',  '.',  '/',
};

static const char keymap_shift[] = {
    0,    0,    0,    0,
    'A',  'B',  'C',  'D',
    'E',  'F',  'G',  'H',
    'I',  'J',  'K',  'L',
    'M',  'N',  'O',  'P',
    'Q',  'R',  'S',  'T',
    'U',  'V',  'W',  'X',
    'Y',  'Z',
    '!',  '@',  '#',  '$',  '%',
    '^',  '&',  '*',  '(',  ')',
    '\r', '\x1b','\x7f','\t',' ',
    '_',  '+',  '{',  '}',  '|',
    0,    ':',  '"',  '~',
    '<',  '>',  '?',
};

#define MOD_SHIFT  (0x02 | 0x20)
#define MOD_CTRL   (0x01 | 0x10)

static uint8_t s_prev_keys[6];

static void send_vt100(const char *seq)
{
    while (*seq) xQueueSend(s_key_queue, seq++, 0);
}

static void translate_report(const uint8_t *report, int len)
{
    if (len < 3) return;
    uint8_t mod = report[0];
    bool shifted = (mod & MOD_SHIFT) != 0;
    bool ctrl    = (mod & MOD_CTRL)  != 0;

    uint8_t cur_keys[6] = {0};
    for (int i = 0; i < 6 && (i + 1) < len; i++)
        cur_keys[i] = report[i + 1];

    for (int i = 0; i < 6; i++) {
        uint8_t code = cur_keys[i];
        if (code == 0) continue;

        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (s_prev_keys[j] == code) { was_pressed = true; break; }
        }
        if (was_pressed) continue;

        ESP_LOGD(TAG, "key press: code=0x%02x mod=0x%02x", code, mod);

        if (code == 0x4F) { send_vt100("\x1b[C"); continue; }
        if (code == 0x50) { send_vt100("\x1b[D"); continue; }
        if (code == 0x51) { send_vt100("\x1b[B"); continue; }
        if (code == 0x52) { send_vt100("\x1b[A"); continue; }

        char ch = 0;
        if (code < sizeof(keymap_normal))
            ch = shifted ? keymap_shift[code] : keymap_normal[code];

        if (ctrl && ch >= 'a' && ch <= 'z') ch -= 96;
        if (ctrl && ch >= 'A' && ch <= 'Z') ch -= 64;

        if (ch) xQueueSend(s_key_queue, &ch, 0);
    }

    memcpy(s_prev_keys, cur_keys, 6);
}

// -- Connection watchdog ------------------------------------------------------

static bool try_bonded_reconnect(bool show_on_display);

static void connection_watchdog_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_disconnected_sem, portMAX_DELAY);
        s_reconnecting = true;

        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_display)
            display_show_status(s_display, "kbd disconnected", "switch back to reconnect");

        if (s_hidh_dev) {
            for (int attempt = 1; ; attempt++) {
                ESP_LOGI(TAG, "Light reconnect attempt %d", attempt);
                esp_hidh_dev_t *dev = esp_ble_hidh_dev_reconnect(s_hidh_dev);
                if (dev) {
                    ESP_LOGI(TAG, "Reconnected successfully");
                    break;
                }
                ESP_LOGW(TAG, "Reconnect attempt %d failed, retrying...", attempt);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        } else {
            while (!try_bonded_reconnect(false)) {
                ble_gap_conn_cancel();
                ESP_LOGW(TAG, "Reconnect failed, retrying in 3s...");
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(300));
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        char discard;
        while (xQueueReceive(s_key_queue, &discard, 0) == pdTRUE)
            ;

        s_reconnecting = false;

        char wakeup = '\0';
        xQueueSend(s_key_queue, &wakeup, 0);
    }
}

// -- HIDH event callback ------------------------------------------------------

static void hidh_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            ESP_LOGI(TAG, "keyboard connected: %s", esp_hidh_dev_name_get(p->open.dev));
            {
                int bc = 0;
                ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bc);
                ESP_LOGI(TAG, "After connect: bond_count=%d sm_bonding=%d store_write_cb=%s",
                         bc, ble_hs_cfg.sm_bonding,
                         ble_hs_cfg.store_write_cb ? "set" : "NULL");
            }
            s_hidh_dev = p->open.dev;
            {
                esp_hidh_dev_t *dev = p->open.dev;
                struct ble_gap_upd_params params = {
                    .itvl_min = 6,
                    .itvl_max = 24,
                    .latency  = 4,
                    .supervision_timeout = 500,
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                int rc2 = ble_gap_update_params(dev->ble.conn_id, &params);
                ESP_LOGI(TAG, "Set supervision timeout: rc=%d conn=%d", rc2, dev->ble.conn_id);
            }
            if (s_display && !s_reconnecting) {
                display_show_status(s_display, "kbd connected!", "");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            xSemaphoreGive(s_connected_sem);
        } else {
            ESP_LOGE(TAG, "keyboard open failed: %d", p->open.status);
        }
        xSemaphoreGive(s_open_done_sem);
        break;
    case ESP_HIDH_INPUT_EVENT:
        ESP_LOGD(TAG, "HID input: usage=%d map=%d id=%d len=%d",
                 p->input.usage, p->input.map_index, p->input.report_id,
                 p->input.length);
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD)
            translate_report(p->input.data, p->input.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "keyboard disconnected, will reconnect");
        xSemaphoreTake(s_connected_sem, 0);
        xSemaphoreGive(s_disconnected_sem);
        break;
    default:
        break;
    }
}

// -- NimBLE host task ---------------------------------------------------------

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// -- BLE scan: collect all connectable named devices --------------------------

#define MAX_SCAN_DEVS 8

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char    name[33];
    int8_t  rssi;
} bt_scan_dev_t;

static bt_scan_dev_t s_scan_devs[MAX_SCAN_DEVS];
static int           s_scan_dev_count;

static int collect_scan_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_gap_disc_desc *d = &event->disc;

    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) return 0;
    if (fields.name_len == 0) return 0;

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    // Update existing entry or add new one
    for (int i = 0; i < s_scan_dev_count; i++) {
        if (memcmp(s_scan_devs[i].addr, d->addr.val, 6) == 0) {
            s_scan_devs[i].rssi = d->rssi;
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
    }

    if (s_scan_dev_count < MAX_SCAN_DEVS) {
        bt_scan_dev_t *dev = &s_scan_devs[s_scan_dev_count];
        memcpy(dev->addr, d->addr.val, 6);
        dev->addr_type = d->addr.type;
        int n = fields.name_len < 32 ? (int)fields.name_len : 32;
        memcpy(dev->name, fields.name, n);
        dev->name[n] = '\0';
        dev->rssi = d->rssi;
        s_scan_dev_count++;
        ESP_LOGI(TAG, "Found: '%s' rssi=%d addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 dev->name, dev->rssi,
                 d->addr.val[5], d->addr.val[4], d->addr.val[3],
                 d->addr.val[2], d->addr.val[1], d->addr.val[0]);
    }

    xSemaphoreGive(s_scan_mutex);
    return 0;
}

// -- Device list UI -----------------------------------------------------------

static void draw_device_list(display_t *d, int cursor, int count,
                              bt_scan_dev_t *devs)
{
    const int char_h = d->geom.char_h;
    const int cols   = d->geom.cols;
    const int rows   = d->geom.rows;

    display_clear(d);

    display_puts(d, 0, 0, "Bluetooth Keyboard Setup");

    // Separator line
    char sep[64];
    int sep_len = cols < 63 ? cols : 63;
    memset(sep, '-', sep_len);
    sep[sep_len] = '\0';
    display_puts(d, 0, 1 * char_h, sep);

    // Device rows start at row 2
    char line[64];
    for (int i = 0; i < count; i++) {
        char rssi_str[16];
        snprintf(rssi_str, sizeof(rssi_str), "%d dB", devs[i].rssi);
        int rssi_len = (int)strlen(rssi_str);
        int name_w   = cols - rssi_len - 4;  // 2 prefix + 2 gap

        snprintf(line, sizeof(line), "%c %-*.*s  %s",
                 (i == cursor) ? '>' : ' ',
                 name_w, name_w, devs[i].name, rssi_str);
        display_puts(d, 0, (i + 2) * char_h, line);
    }

    if (count == 0)
        display_puts(d, 0, 4 * char_h, "Scanning...");

    display_puts(d, 0, (rows - 2) * char_h, "Tap a device to connect");
    display_puts(d, 0, (rows - 1) * char_h, "BOOT: next  |  Hold 1s: connect");

    display_flush(d);
}

// Run the pairing UI. Blocks until the user selects a device.
// Returns a pointer into s_scan_devs (stable until next call).
static bt_scan_dev_t *run_pairing_ui(display_t *d)
{
    struct ble_gap_disc_params disc = {
        .passive           = 0,
        .filter_duplicates = 1,
        .itvl              = 0x0050,
        .window            = 0x0030,
    };

restart_scan:
    s_scan_dev_count = 0;
    memset(s_scan_devs, 0, sizeof(s_scan_devs));

    int scan_rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc,
                               collect_scan_cb, NULL);
    if (scan_rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d, retrying in 1s", scan_rc);
        vTaskDelay(pdMS_TO_TICKS(1000));
        goto restart_scan;
    }

    int cursor   = 0;
    int held_ms  = 0;
    TickType_t scan_start  = xTaskGetTickCount();
    TickType_t last_redraw = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // --- Touch input ---
        uint16_t tx, ty;
        int tapped_dev = -1;
        if (touch_poll_tap(&tx, &ty)) {
            int row = (ty - d->geom.margin_y) / d->geom.char_h;
            int idx = row - 2;
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            int cnt = s_scan_dev_count;
            xSemaphoreGive(s_scan_mutex);
            if (idx >= 0 && idx < cnt)
                tapped_dev = idx;
        }

        // --- BOOT button ---
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 1000) {
                // Held 1s: select at cursor
                xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                int cnt = s_scan_dev_count;
                xSemaphoreGive(s_scan_mutex);
                if (cnt > 0) {
                    tapped_dev = cursor;
                    held_ms = 0;
                }
            }
        } else {
            if (held_ms > 0 && held_ms < 1000) {
                // Short press: cycle cursor
                xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                int cnt = s_scan_dev_count;
                xSemaphoreGive(s_scan_mutex);
                if (cnt > 0)
                    cursor = (cursor + 1) % cnt;
            }
            held_ms = 0;
        }

        // --- Auto-connect if only one device after 8s ---
        {
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            int cnt = s_scan_dev_count;
            xSemaphoreGive(s_scan_mutex);
            uint32_t elapsed_ms = pdTICKS_TO_MS(now - scan_start);
            if (cnt == 1 && elapsed_ms > 8000 && tapped_dev == -1)
                tapped_dev = 0;
        }

        // --- Handle selection with 1.5s countdown ---
        if (tapped_dev >= 0) {
            ble_gap_disc_cancel();

            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            // tapped_dev is valid because we checked cnt above
            bt_scan_dev_t dev_copy = s_scan_devs[tapped_dev];
            static bt_scan_dev_t s_selected;
            s_selected = dev_copy;
            xSemaphoreGive(s_scan_mutex);

            char countdown[32];
            for (int i = 3; i > 0; i--) {
                snprintf(countdown, sizeof(countdown), "Connecting in %d...", i);
                if (d) display_show_status(d, s_selected.name, countdown);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            return &s_selected;
        }

        // --- No devices after 20s: show prompt and restart ---
        {
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            int cnt = s_scan_dev_count;
            xSemaphoreGive(s_scan_mutex);
            uint32_t elapsed_ms = pdTICKS_TO_MS(now - scan_start);
            if (cnt == 0 && elapsed_ms > 20000) {
                ble_gap_disc_cancel();
                if (d) display_show_status(d, "Put keyboard in", "pairing mode");
                vTaskDelay(pdMS_TO_TICKS(3000));
                scan_start = xTaskGetTickCount();
                goto restart_scan;
            }
        }

        // --- Redraw every 500ms ---
        if (pdTICKS_TO_MS(now - last_redraw) >= 500) {
            last_redraw = now;
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            int cnt = s_scan_dev_count;
            bt_scan_dev_t devs_copy[MAX_SCAN_DEVS];
            memcpy(devs_copy, s_scan_devs, sizeof(bt_scan_dev_t) * cnt);
            xSemaphoreGive(s_scan_mutex);
            if (d) draw_device_list(d, cursor, cnt, devs_copy);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// -- Try reconnecting to a bonded peer ----------------------------------------

static bool try_bonded_reconnect(bool show_on_display)
{
    int bond_count = 0;
    int rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bond_count);
    ESP_LOGI(TAG, "Bond store: count=%d rc=%d", bond_count, rc);
    if (bond_count == 0) {
        ESP_LOGI(TAG, "No bonded peers, need fresh pairing");
        return false;
    }

    struct ble_store_key_sec key = { .peer_addr = *BLE_ADDR_ANY, .idx = 0 };
    struct ble_store_value_sec value;
    rc = ble_store_read_peer_sec(&key, &value);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to read bonded peer info: rc=%d", rc);
        return false;
    }

    ESP_LOGI(TAG, "Bonded peer: %02x:%02x:%02x:%02x:%02x:%02x (type=%d)",
             value.peer_addr.val[5], value.peer_addr.val[4],
             value.peer_addr.val[3], value.peer_addr.val[2],
             value.peer_addr.val[1], value.peer_addr.val[0],
             value.peer_addr.type);

    if (show_on_display && s_display)
        display_show_status(s_display, "reconnecting...", "Hold BOOT 2s to re-pair");

    uint8_t addr_type = value.peer_addr.type;
    if (addr_type == BLE_ADDR_PUBLIC)
        addr_type = BLE_ADDR_PUBLIC_ID;
    else if (addr_type == BLE_ADDR_RANDOM)
        addr_type = BLE_ADDR_RANDOM_ID;

    xSemaphoreTake(s_open_done_sem, 0);
    esp_hidh_dev_open(value.peer_addr.val, ESP_HID_TRANSPORT_BLE, addr_type);

    // Wait up to 15s for OPEN_EVENT, polling BOOT every 100ms for re-pair trigger
    int held_ms = 0;
    for (int waited = 0; waited < 15000; waited += 100) {
        if (xSemaphoreTake(s_open_done_sem, pdMS_TO_TICKS(100)) == pdTRUE)
            break;
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 2000) {
                ESP_LOGW(TAG, "BOOT held 2s during reconnect — forcing re-pair");
                ble_gap_conn_cancel();
                ble_store_clear();
                s_force_repair = true;
                return false;
            }
        } else {
            held_ms = 0;
        }
    }

    if (xSemaphoreTake(s_connected_sem, 0) == pdTRUE) {
        xSemaphoreGive(s_connected_sem);
        return true;
    }

    ESP_LOGW(TAG, "Bonded reconnect timed out");
    ble_gap_conn_cancel();
    return false;
}

// -- Scan + connect task ------------------------------------------------------

static void scan_task(void *arg)
{
    // Configure BOOT button GPIO
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&boot_cfg);

    bool force_repair = s_force_repair;

    // Try bonded reconnect unless force-repairing
    if (!force_repair) {
        int bond_count = 0;
        ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bond_count);

        if (bond_count > 0) {
            bool first = true;
            while (1) {
                ESP_LOGI(TAG, "Bonded reconnect attempt...");
                if (try_bonded_reconnect(first)) {
                    xTaskCreate(connection_watchdog_task, "bt_watch", 16384, NULL, 2, NULL);
                    vTaskDelete(NULL);
                    return;
                }
                first = false;

                // try_bonded_reconnect sets s_force_repair if BOOT was held
                if (s_force_repair) {
                    force_repair = true;
                    break;
                }

                // Otherwise wait 3s between attempts, still watching BOOT
                ESP_LOGW(TAG, "Bonded reconnect failed, retrying in 3s (hold BOOT 2s to re-pair)");
                int held_ms = 0;
                for (int elapsed = 0; elapsed < 3000; elapsed += 100) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        held_ms += 100;
                        if (held_ms >= 2000) {
                            ESP_LOGW(TAG, "BOOT held 2s — clearing bonds, forcing re-pair");
                            ble_store_clear();
                            force_repair = true;
                            s_force_repair = true;
                            break;
                        }
                    } else {
                        held_ms = 0;
                    }
                }
                if (force_repair) break;
            }
        }
    }

    // Fresh pairing: show device discovery UI
    while (1) {
        bt_scan_dev_t *selected = run_pairing_ui(s_display);
        if (!selected) continue;

        // Let scan cancel settle
        vTaskDelay(pdMS_TO_TICKS(500));

        xSemaphoreTake(s_open_done_sem, 0);
        esp_hidh_dev_open(selected->addr, ESP_HID_TRANSPORT_BLE, selected->addr_type);

        // Wait up to 60s (passkey flow can take time)
        if (xSemaphoreTake(s_connected_sem, pdMS_TO_TICKS(60000)) == pdTRUE) {
            xSemaphoreGive(s_connected_sem);
            xTaskCreate(connection_watchdog_task, "bt_watch", 16384, NULL, 2, NULL);
            break;
        }

        ESP_LOGW(TAG, "Connection failed, returning to device list");
        if (s_display) display_show_status(s_display, "Connection failed", "retrying...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

// -- Public API ---------------------------------------------------------------

void bt_kbd_wait_connected(void)
{
    xSemaphoreTake(s_connected_sem, portMAX_DELAY);
}

QueueHandle_t bt_kbd_init(display_t *display, bool force_repair)
{
    s_display     = display;
    s_force_repair = force_repair;
    s_key_queue      = xQueueCreate(64, sizeof(char));
    s_connected_sem  = xSemaphoreCreateBinary();
    s_disconnected_sem = xSemaphoreCreateBinary();
    s_open_done_sem  = xSemaphoreCreateBinary();
    s_scan_mutex     = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));

    esp_hidh_config_t cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&cfg));

    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;
    ble_store_config_init();

    esp_hid_gap_set_passkey_cb(show_passkey);
    extern void esp_ble_hidh_set_passkey_cb(void (*cb)(uint32_t passkey));
    esp_ble_hidh_set_passkey_cb(show_passkey);

    ESP_ERROR_CHECK(esp_nimble_enable(nimble_host_task));

    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_force_repair) {
        ESP_LOGW(TAG, "Force re-pair: clearing bonds");
        ble_store_clear();
    }

    xTaskCreate(scan_task, "bt_scan", 4096, NULL, 2, NULL);

    return s_key_queue;
}
