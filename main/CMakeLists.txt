set(SRCS "main.c" "wifi_time.c"
         "term.c" "ssh_targets.c" "menu.c"
         "display_mipi_dsi.c" "slave_ota.c" "touch.c")

if(CONFIG_BT_ENABLED)
    list(APPEND SRCS "bt_kbd_ui.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
                 esp_lcd esp_psram
                 esp_mm espressif__esp_hosted bootloader_support
                 esp_app_format esp_partition libvterm
                 esp_lcd_st7703 bt esp-ble-kbd-host
    EMBED_TXTFILES "${PROJECT_DIR}/keys/id_ed25519"
                   "${PROJECT_DIR}/keys/id_ed25519.pub"
)
