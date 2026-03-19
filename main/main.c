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

    // Show splash screen during boot
    display_show_splash(&display);

#if CONFIG_BOARD_P4_WAVESHARE
    // TODO: esp_hosted crashes on this board — need to investigate C6 firmware
    // For now, display test only
    vTaskDelay(pdMS_TO_TICKS(2000));
    display_clear(&display);
    display_puts(&display, 0, 0,                          "Blokyo Terminal");
    display_puts(&display, 0, display.geom.char_h,        "Waveshare 720x720");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%dx%d grid", display.geom.cols, display.geom.rows);
        display_puts(&display, 0, 2 * display.geom.char_h, buf);
    }
    display_puts(&display, 0, 4 * display.geom.char_h,    "C6 hosted not yet configured");
    display_flush(&display);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

    // TODO: find correct boot button GPIO on P4 board (GPIO 0 floats low)
    bool force_repair = false;

    // Connect to ESP32-C6 co-processor via SDIO (provides WiFi + BLE)
    ESP_LOGI(TAG, "Connecting to co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor connected");

    // Update C6 co-processor firmware if needed (old FW doesn't support BT RPC)
    slave_ota_update_if_needed();

    // WiFi + NTP (also inits NVS, netif, event loop)
    wifi_time_sync();

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
