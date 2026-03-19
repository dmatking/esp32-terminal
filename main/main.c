#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "display.h"
#include "wifi_time.h"
#include "ssh_term.h"
#include "menu.h"
#include "esp_hosted.h"
#include "slave_ota.h"

#if CONFIG_BT_ENABLED
#include "bt_kbd.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    static display_t display;
    static mipi_dsi_priv_t hw_dsi;

    ESP_ERROR_CHECK(display_mipi_dsi_init(&display, &hw_dsi));

    // Show splash screen during boot
    display_show_splash(&display);

    bool force_repair = false;  // BOOT long-press is now handled in bt_kbd scan task

    // Connect to ESP32-C6 co-processor via SDIO (provides WiFi + BLE)
    ESP_LOGI(TAG, "Connecting to co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor connected");

    // Update C6 co-processor firmware if needed (old FW doesn't support BT RPC)
    slave_ota_update_if_needed();

    // WiFi + NTP (also inits NVS, netif, event loop)
    wifi_time_sync();

    QueueHandle_t keys = NULL;

#if CONFIG_BT_ENABLED
    // BLE keyboard (NimBLE HID Host)
    keys = bt_kbd_init(&display, force_repair);
    ESP_LOGI(TAG, "BT keyboard init done");

    // Wait for keyboard to actually connect before starting SSH
    bt_kbd_wait_connected();
    ESP_LOGI(TAG, "Keyboard connected");
#else
    // No BT — create a key queue for future use (e.g. touch input)
    keys = xQueueCreate(64, sizeof(char));
    ESP_LOGW(TAG, "BT disabled — no keyboard input");

    display_clear(&display);
    display_puts(&display, 0, 0,                          "WiFi connected");
    display_puts(&display, 0, display.geom.char_h,        "BT disabled (C6 FW too old)");
    display_puts(&display, 0, 3 * display.geom.char_h,    "SSH needs keyboard input");
    display_flush(&display);
#endif

    // Init wolfSSH once
    ssh_term_init();

    // Hand off to menu -- does not return
    menu_run(&display, keys);
}
