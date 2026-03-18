#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "display.h"
#include "wifi_time.h"
#include "bt_kbd.h"
#include "ssh_term.h"
#include "menu.h"
#include "esp_hosted.h"
#include "slave_ota.h"

static const char *TAG = "main";

void app_main(void)
{
    static display_t display;
    static mipi_dsi_priv_t hw_dsi;

    ESP_ERROR_CHECK(display_mipi_dsi_init(&display, &hw_dsi));

    // TODO: find correct boot button GPIO on P4 board (GPIO 0 floats low)
    bool force_repair = false;

    display_clear(&display);
    display_puts(&display, 0, 0, "terminal p4");
    display_puts(&display, 0, display.geom.char_h, "esp_hosted...");
    display_flush(&display);

    // Connect to ESP32-C6 co-processor via SDIO (provides WiFi + BLE)
    ESP_LOGI(TAG, "Connecting to co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor connected");

    // Update C6 co-processor firmware if needed (old FW doesn't support BT RPC)
    display_puts(&display, 0, display.geom.char_h, "c6 firmware...   ");
    display_flush(&display);
    slave_ota_update_if_needed();

    display_puts(&display, 0, display.geom.char_h, "wifi...          ");
    display_flush(&display);

    // WiFi + NTP (also inits NVS, netif, event loop)
    wifi_time_sync();

    display_puts(&display, 0, display.geom.char_h, "bt keyboard...");
    display_flush(&display);

    // BLE keyboard (NimBLE HID Host)
    QueueHandle_t keys = bt_kbd_init(&display, force_repair);
    ESP_LOGI(TAG, "BT keyboard init done");

    // Wait for keyboard to actually connect before starting SSH
    bt_kbd_wait_connected();
    ESP_LOGI(TAG, "Keyboard connected");

    // Init wolfSSH once
    ssh_term_init();

    // Hand off to menu -- does not return
    menu_run(&display, keys);
}
