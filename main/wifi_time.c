#include "wifi_time.h"
#include "wifi_creds.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <time.h>

#define CONNECTED_BIT  BIT0

static const char *TAG = "wifi_time";
static EventGroupHandle_t s_events;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Auto-reconnect so SNTP keeps syncing
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP");
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

void wifi_time_sync(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS issue (%s), erasing...", esp_err_to_name(ret));
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "NVS init: %s", esp_err_to_name(ret));

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    s_events = xEventGroupCreate();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid      = WIFI_SSID,
            .password  = WIFI_PASS,
            .threshold = { .authmode = WIFI_AUTH_WPA2_PSK },
            .pmf_cfg   = { .capable = true, .required = false },
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);

    // Wait up to 30s for initial connection
    EventBits_t bits = xEventGroupWaitBits(s_events,
        CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & CONNECTED_BIT) {
        setenv("TZ", WIFI_TZ, 1);
        tzset();

        // Start SNTP -- stays running, auto-resyncs every hour
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();

        ESP_LOGI(TAG, "Waiting for NTP sync...");
        time_t now = 0;
        int retries = 0;
        while (time(&now) < 1577836800L && retries++ < 100)
            vTaskDelay(pdMS_TO_TICKS(100));

        if (now >= 1577836800L)
            ESP_LOGI(TAG, "Time synced -- WiFi + SNTP staying active");
        else
            ESP_LOGW(TAG, "NTP sync timed out");
    } else {
        ESP_LOGW(TAG, "WiFi connect timed out");
    }

    // WiFi and SNTP remain running -- SNTP auto-resyncs hourly
}
