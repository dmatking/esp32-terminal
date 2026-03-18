#include "bt_kbd.h"
#include "bt_creds.h"
#include "display.h"
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
#include <stdio.h>
#include <string.h>

// No public header for ble_store_config_init -- declared as extern per NimBLE convention
extern void ble_store_config_init(void);
// Lightweight reconnect (our addition to nimble_hidh.c)
extern esp_hidh_dev_t *esp_ble_hidh_dev_reconnect(esp_hidh_dev_t *dev);

static const char *TAG = "bt_kbd";

static QueueHandle_t s_key_queue;
static display_t *s_display;
static SemaphoreHandle_t s_connected_sem;
static SemaphoreHandle_t s_disconnected_sem;  // signaled on disconnect
static SemaphoreHandle_t s_open_done_sem;     // signaled when OPEN_EVENT fires (success or fail)
static bool s_reconnecting;  // true during background reconnect
static bool s_force_repair;  // set by bt_kbd_init if boot button was held
static esp_hidh_dev_t *s_hidh_dev;  // current HID device for reconnect

// -- Passkey display ----------------------------------------------------------

static void show_passkey(uint32_t key)
{
    if (!s_display) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)key);
    display_clear(s_display);
    display_puts(s_display, 0,  0 * s_display->geom.char_h, "BT pairing:");
    display_puts(s_display, 0,  2 * s_display->geom.char_h, "type on keyboard");
    display_puts(s_display, 0,  4 * s_display->geom.char_h, "then Enter:");
    display_puts(s_display, 16, 6 * s_display->geom.char_h, buf);
    display_flush(s_display);
}

// -- HID keycode -> ASCII -----------------------------------------------------

// HID Usage ID -> unshifted ASCII (index = usage id, offset 0)
static const char keymap_normal[] = {
    0,    0,    0,    0,           // 0x00-0x03
    'a',  'b',  'c',  'd',         // 0x04-0x07
    'e',  'f',  'g',  'h',         // 0x08-0x0B
    'i',  'j',  'k',  'l',         // 0x0C-0x0F
    'm',  'n',  'o',  'p',         // 0x10-0x13
    'q',  'r',  's',  't',         // 0x14-0x17
    'u',  'v',  'w',  'x',         // 0x18-0x1B
    'y',  'z',                     // 0x1C-0x1D
    '1',  '2',  '3',  '4',  '5',   // 0x1E-0x22
    '6',  '7',  '8',  '9',  '0',   // 0x23-0x27
    '\r', '\x1b','\x7f','\t',' ',  // 0x28 Enter, 0x29 Esc, 0x2A BS, 0x2B Tab, 0x2C Space
    '-',  '=',  '[',  ']',  '\\',  // 0x2D-0x31
    0,    ';',  '\'', '`',          // 0x32-0x35
    ',',  '.',  '/',                // 0x36-0x38
};

// HID Usage ID -> shifted ASCII
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

// Track previous report to detect new key presses only
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

    // Extract current keycodes -- this keyboard sends 7-byte reports
    // with no reserved byte: [mod, key1, key2, key3, key4, key5, key6]
    uint8_t cur_keys[6] = {0};
    for (int i = 0; i < 6 && (i + 1) < len; i++)
        cur_keys[i] = report[i + 1];

    // Only process keys that are NEW (not in previous report)
    for (int i = 0; i < 6; i++) {
        uint8_t code = cur_keys[i];
        if (code == 0) continue;

        // Check if this key was already in the previous report
        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (s_prev_keys[j] == code) { was_pressed = true; break; }
        }
        if (was_pressed) continue;

        ESP_LOGD(TAG, "key press: code=0x%02x mod=0x%02x", code, mod);

        // Arrow keys -> VT100 sequences
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

    // Save current state for next report
    memcpy(s_prev_keys, cur_keys, 6);
}

// -- Connection watchdog (persistent task) ------------------------------------

static bool try_bonded_reconnect(bool show_on_display);  // forward decl

static void connection_watchdog_task(void *arg)
{
    while (1) {
        // Wait for a disconnect event
        xSemaphoreTake(s_disconnected_sem, portMAX_DELAY);
        s_reconnecting = true;

        // Brief delay to let disconnect settle
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Show status on display
        if (s_display) {
            display_clear(s_display);
            display_puts(s_display, 0, 0 * s_display->geom.char_h, "kbd disconnected");
            display_puts(s_display, 0, 2 * s_display->geom.char_h, "switch keyboard");
            display_puts(s_display, 0, 3 * s_display->geom.char_h, "back to reconnect");
            display_flush(s_display);
        }

        // Use lightweight reconnect if we have a device (skip GATT rediscovery)
        if (s_hidh_dev) {
            for (int attempt = 1; ; attempt++) {
                ESP_LOGI(TAG, "Light reconnect attempt %d", attempt);
                esp_hidh_dev_t *dev = esp_ble_hidh_dev_reconnect(s_hidh_dev);
                if (dev) {
                    ESP_LOGI(TAG, "Reconnected successfully");
                    break;
                }
                ble_gap_conn_cancel();
                ESP_LOGW(TAG, "Reconnect attempt %d failed, retrying in 3s...", attempt);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        } else {
            // Fallback: full bonded reconnect (first boot, no device yet)
            while (!try_bonded_reconnect(false)) {
                ble_gap_conn_cancel();
                ESP_LOGW(TAG, "Reconnect failed, retrying in 3s...");
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }

        // Drain any keys queued during the reconnect (e.g. Easy-Switch button
        // press that triggered the switch-back).
        vTaskDelay(pdMS_TO_TICKS(300));
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        char discard;
        while (xQueueReceive(s_key_queue, &discard, 0) == pdTRUE)
            ;

        s_reconnecting = false;

        // Send a NUL sentinel to wake the menu out of read_key() so it
        // redraws immediately. The SSH keyboard task filters NUL out.
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
            // Save device pointer for reconnect
            s_hidh_dev = p->open.dev;
            // Set a 5s supervision timeout so we detect keyboard switching away
            {
                esp_hidh_dev_t *dev = p->open.dev;
                struct ble_gap_upd_params params = {
                    .itvl_min = 6,    // 7.5ms (units of 1.25ms)
                    .itvl_max = 24,   // 30ms
                    .latency  = 4,    // skip up to 4 intervals
                    .supervision_timeout = 500,  // 5s (units of 10ms)
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                int rc2 = ble_gap_update_params(dev->ble.conn_id, &params);
                ESP_LOGI(TAG, "Set supervision timeout: rc=%d conn=%d", rc2, dev->ble.conn_id);
            }
            if (s_display && !s_reconnecting) {
                display_clear(s_display);
                display_puts(s_display, 0, 0, "kbd connected!");
                display_flush(s_display);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            xSemaphoreGive(s_connected_sem);
        } else {
            ESP_LOGE(TAG, "keyboard open failed: %d", p->open.status);
        }
        xSemaphoreGive(s_open_done_sem);
        break;
    case ESP_HIDH_INPUT_EVENT:
        ESP_LOGI(TAG, "HID input: usage=%d map=%d id=%d len=%d data=%02x %02x %02x %02x %02x %02x %02x %02x",
                 p->input.usage, p->input.map_index, p->input.report_id,
                 p->input.length,
                 p->input.length > 0 ? p->input.data[0] : 0,
                 p->input.length > 1 ? p->input.data[1] : 0,
                 p->input.length > 2 ? p->input.data[2] : 0,
                 p->input.length > 3 ? p->input.data[3] : 0,
                 p->input.length > 4 ? p->input.data[4] : 0,
                 p->input.length > 5 ? p->input.data[5] : 0,
                 p->input.length > 6 ? p->input.data[6] : 0,
                 p->input.length > 7 ? p->input.data[7] : 0);
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD)
            translate_report(p->input.data, p->input.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "keyboard disconnected, will reconnect");
        xSemaphoreTake(s_connected_sem, 0);  // clear connected state
        xSemaphoreGive(s_disconnected_sem);   // wake watchdog task
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

// -- BLE scan callbacks -------------------------------------------------------

// Scan-mode: print everything
static int raw_scan_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_gap_disc_desc *d = &event->disc;
    char name[33] = "?";
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0 && fields.name_len) {
        int n = fields.name_len < 32 ? fields.name_len : 32;
        memcpy(name, fields.name, n);
        name[n] = '\0';
    }
    const char *evt_str = "?";
    switch (d->event_type) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:      evt_str = "CONN"; break;
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:       evt_str = "DIRECT"; break;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:      evt_str = "SCAN_ONLY"; break;
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:   evt_str = "NONCONN"; break;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:      evt_str = "SCAN_RSP"; break;
        default: break;
    }
    ESP_LOGI(TAG, "  BLE %02x:%02x:%02x:%02x:%02x:%02x atype=%d evt=%s rssi=%d name=%s",
             d->addr.val[5], d->addr.val[4], d->addr.val[3],
             d->addr.val[2], d->addr.val[1], d->addr.val[0],
             d->addr.type, evt_str, d->rssi, name);
    return 0;
}

// Direct-connect mode: watch for the target name, capture its address
static uint8_t s_found_addr[6];   // NimBLE byte order (LSB-first)
static uint8_t s_found_addr_type;

static int connect_scan_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_gap_disc_desc *d = &event->disc;

    // Only consider connectable advertisements
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return 0;

    // Check name in advertising data
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) return 0;
    if (fields.name_len == 0) return 0;

    if (fields.name_len < strlen(BT_KBD_NAME)) return 0;
    if (memcmp(fields.name, BT_KBD_NAME, strlen(BT_KBD_NAME)) != 0) return 0;

    // Match! Save the address
    memcpy(s_found_addr, d->addr.val, 6);
    s_found_addr_type = d->addr.type;

    ESP_LOGI(TAG, "Keyboard '%s' found at %02x:%02x:%02x:%02x:%02x:%02x (type=%d)",
             BT_KBD_NAME,
             d->addr.val[5], d->addr.val[4], d->addr.val[3],
             d->addr.val[2], d->addr.val[1], d->addr.val[0],
             d->addr.type);
    ble_gap_disc_cancel();
    xSemaphoreGive((SemaphoreHandle_t)arg);
    return 0;
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

    // Read first bonded peer's identity address
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

    if (show_on_display && s_display) {
        display_clear(s_display);
        display_puts(s_display, 0, 0 * s_display->geom.char_h, "reconnecting...");
        display_puts(s_display, 0, 1 * s_display->geom.char_h, "press any key on");
        display_puts(s_display, 0, 2 * s_display->geom.char_h, "keyboard to connect");
        display_puts(s_display, 0, 4 * s_display->geom.char_h, "hold BOOT on reset");
        display_puts(s_display, 0, 5 * s_display->geom.char_h, "to re-pair");
        display_flush(s_display);
    }

    // Use the identity address type with ID flag so NimBLE resolves RPAs
    uint8_t addr_type = value.peer_addr.type;
    if (addr_type == BLE_ADDR_PUBLIC)
        addr_type = BLE_ADDR_PUBLIC_ID;
    else if (addr_type == BLE_ADDR_RANDOM)
        addr_type = BLE_ADDR_RANDOM_ID;

    // Clear the open-done semaphore before starting
    xSemaphoreTake(s_open_done_sem, 0);
    esp_hidh_dev_open(value.peer_addr.val, ESP_HID_TRANSPORT_BLE, addr_type);

    // Wait for OPEN_EVENT (success or fail) -- NimBLE connection timeout is ~30s
    xSemaphoreTake(s_open_done_sem, pdMS_TO_TICKS(35000));

    // Check if we actually connected
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
#if BT_KBD_SCAN_MODE
    if (s_display) {
        display_clear(s_display);
        display_puts(s_display, 0, 0 * s_display->geom.char_h, "Put keyboard in");
        display_puts(s_display, 0, 1 * s_display->geom.char_h, "pairing mode NOW");
        display_puts(s_display, 0, 2 * s_display->geom.char_h, "K380: hold F1-3");
        display_puts(s_display, 0, 3 * s_display->geom.char_h, "MX: hold Easy-");
        display_puts(s_display, 0, 4 * s_display->geom.char_h, "Switch 3 sec");
        display_puts(s_display, 0, 6 * s_display->geom.char_h, "scanning in 15s..");
        display_flush(s_display);
    }
    vTaskDelay(pdMS_TO_TICKS(15000));

    if (s_display) {
        display_clear(s_display);
        display_puts(s_display, 0, 0 * s_display->geom.char_h, "scanning BLE...");
        display_puts(s_display, 0, 1 * s_display->geom.char_h, "(30s, check log)");
        display_flush(s_display);
    }

    struct ble_gap_disc_params disc = {
        .passive           = 0,
        .filter_duplicates = 1,
        .itvl              = 0x0050,
        .window            = 0x0030,
    };
    ESP_LOGI(TAG, "Raw BLE scan starting (30s) -- all devices:");
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 30000, &disc, raw_scan_cb, NULL);
    vTaskDelay(pdMS_TO_TICKS(31000));
    ESP_LOGI(TAG, "Scan complete.");

    if (s_display) {
        display_clear(s_display);
        display_puts(s_display, 0, 0 * s_display->geom.char_h, "scan done");
        display_puts(s_display, 0, 1 * s_display->geom.char_h, "check serial log");
        display_flush(s_display);
    }
#else
    // If boot button was held at startup, bonds were already cleared in bt_kbd_init
    bool force_repair = s_force_repair;

    // Try reconnecting to a previously bonded keyboard
    if (!force_repair && try_bonded_reconnect(true)) {
        // Start watchdog for future disconnects, then exit scan task
        xTaskCreate(connection_watchdog_task, "bt_watch", 16384, NULL, 2, NULL);
        vTaskDelete(NULL);
        return;
    }
    if (!force_repair)
        ESP_LOGI(TAG, "Bonded reconnect failed, starting fresh pairing scan");

    struct ble_gap_disc_params disc = {
        .passive           = 0,
        .filter_duplicates = 1,
        .itvl              = 0x0050,
        .window            = 0x0030,
    };

    // Retry loop: scan by name -> connect with discovered address
    while (1) {
        if (s_display) {
            display_clear(s_display);
            display_puts(s_display, 0, 0 * s_display->geom.char_h, "Hold Easy-Switch");
            display_puts(s_display, 0, 1 * s_display->geom.char_h, "button 3 sec to");
            display_puts(s_display, 0, 2 * s_display->geom.char_h, "pair keyboard");
            display_flush(s_display);
        }

        SemaphoreHandle_t found = xSemaphoreCreateBinary();
        ESP_LOGI(TAG, "Scanning for '%s'...", BT_KBD_NAME);
        int scan_rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc, connect_scan_cb, found);
        if (scan_rc != 0) {
            ESP_LOGW(TAG, "ble_gap_disc failed: %d, retrying in 3s", scan_rc);
            vSemaphoreDelete(found);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        xSemaphoreTake(found, portMAX_DELAY);
        vSemaphoreDelete(found);

        // Let the scan cancel fully settle
        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_display) {
            display_clear(s_display);
            display_puts(s_display, 0, 0 * s_display->geom.char_h, "kbd found!");
            display_puts(s_display, 0, 1 * s_display->geom.char_h, "connecting...");
            display_flush(s_display);
        }
        // Connect using the address discovered by scan
        esp_hidh_dev_open(s_found_addr, ESP_HID_TRANSPORT_BLE, s_found_addr_type);

        // Wait for OPEN_EVENT -- includes pairing time (user typing passkey)
        if (xSemaphoreTake(s_connected_sem, pdMS_TO_TICKS(60000)) == pdTRUE) {
            xSemaphoreGive(s_connected_sem); // leave set for bt_kbd_wait_connected
            // Start watchdog for future disconnects
            xTaskCreate(connection_watchdog_task, "bt_watch", 16384, NULL, 2, NULL);
            break;
        }
        ESP_LOGW(TAG, "Connection failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    vTaskDelete(NULL);
}

// -- Public API ---------------------------------------------------------------

void bt_kbd_wait_connected(void)
{
    xSemaphoreTake(s_connected_sem, portMAX_DELAY);
}

QueueHandle_t bt_kbd_init(display_t *display, bool force_repair)
{
    s_display = display;
    s_force_repair = force_repair;
    s_key_queue = xQueueCreate(64, sizeof(char));
    s_connected_sem = xSemaphoreCreateBinary();
    s_disconnected_sem = xSemaphoreCreateBinary();
    s_open_done_sem = xSemaphoreCreateBinary();

    // 1. Init BT controller + NimBLE host (esp_hid_gap handles all of this)
    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));

    // 2. Init HID Host
    esp_hidh_config_t cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&cfg));

    // 3. Configure BLE security for passkey display
    //    Must be set before esp_nimble_enable starts the run loop.
    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_DISP_ONLY; // we can show passkey
    ble_hs_cfg.sm_sc           = 1;  // Secure Connections
    ble_hs_cfg.sm_bonding      = 1;  // Bond after pairing
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    // 4. Register our passkey display callback (both GAP and HIDH layers)
    esp_hid_gap_set_passkey_cb(show_passkey);
    extern void esp_ble_hidh_set_passkey_cb(void (*cb)(uint32_t passkey));
    esp_ble_hidh_set_passkey_cb(show_passkey);

    // 5. Start NimBLE run loop
    ESP_ERROR_CHECK(esp_nimble_enable(nimble_host_task));

    // Give NimBLE a moment to sync
    vTaskDelay(pdMS_TO_TICKS(500));

    // 6. Clear bonds if force re-pair was requested (must happen after NimBLE sync)
    if (s_force_repair) {
        ESP_LOGW(TAG, "Force re-pair: clearing bonds");
        ble_store_clear();
    }

    // 7. Start scan/connect task (watchdog starts after first successful connect)
    xTaskCreate(scan_task, "bt_scan", 4096, NULL, 2, NULL);

    return s_key_queue;
}
