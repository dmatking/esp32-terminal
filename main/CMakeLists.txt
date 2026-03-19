set(SRCS "main.c" "wifi_time.c"
         "term.c" "ssh_term.c" "ssh_targets.c" "menu.c"
         "display_mipi_dsi.c" "slave_ota.c")

set(REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
             wolfssl wolfssh esp_lcd esp_psram
             esp_mm espressif__esp_hosted bootloader_support
             esp_app_format esp_partition libvterm
             esp_lcd_st7703)

if(CONFIG_BT_ENABLED)
    list(APPEND SRCS "bt_kbd.c" "esp_hid_gap.c")
    list(APPEND REQUIRES bt esp_hid)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${REQUIRES}
    EMBED_TXTFILES "${PROJECT_DIR}/keys/id_ed25519"
                   "${PROJECT_DIR}/keys/id_ed25519.pub"
    EMBED_FILES "splash_bgr888.bin"
)
