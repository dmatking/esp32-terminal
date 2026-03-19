idf_component_register(
    SRCS "main.c" "wifi_time.c"
         "term.c" "ssh_term.c" "ssh_targets.c" "menu.c"
         "bt_kbd.c" "esp_hid_gap.c"
         "display_mipi_dsi.c" "slave_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
                  bt esp_hid wolfssl wolfssh esp_lcd esp_psram
                  esp_mm espressif__esp_hosted bootloader_support
                  esp_app_format esp_partition libvterm
                  esp_lcd_st7703
    EMBED_TXTFILES "${PROJECT_DIR}/keys/id_ed25519"
                   "${PROJECT_DIR}/keys/id_ed25519.pub"
    EMBED_FILES "splash_bgr888.bin"
)
