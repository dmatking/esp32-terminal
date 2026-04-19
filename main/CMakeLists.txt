set(SRCS "main.c" "wifi_time.c"
         "term.c" "ssh_targets.c" "menu.c"
         "display_mipi_dsi.c" "slave_ota.c" "touch.c")

if(CONFIG_BT_ENABLED)
    list(APPEND SRCS "bt_kbd_ui.c")
endif()

# Locate SSH keys: prefer ~/.esp_keys/, fall back to keys/ in the project tree.
# A fresh clone only needs: mkdir ~/.esp_keys && ssh-keygen -t ed25519 -f ~/.esp_keys/id_ed25519 -N ""
if(EXISTS "$ENV{HOME}/.esp_keys/id_ed25519")
    set(SSH_KEY_DIR "$ENV{HOME}/.esp_keys")
else()
    set(SSH_KEY_DIR "${PROJECT_DIR}/keys")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
                 esp_lcd esp_psram
                 esp_mm espressif__esp_hosted bootloader_support
                 esp_app_format esp_partition libvterm
                 esp_lcd_st7703 bt esp32-ble-kbd-host
    EMBED_TXTFILES "${SSH_KEY_DIR}/id_ed25519"
                   "${SSH_KEY_DIR}/id_ed25519.pub"
)
