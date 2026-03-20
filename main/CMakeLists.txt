set(SRCS "main.c" "wifi_time.c"
         "term.c" "ssh_term.c" "ssh_targets.c" "menu.c"
         "display_mipi_dsi.c" "slave_ota.c")

if(CONFIG_BT_ENABLED)
    list(APPEND SRCS "bt_kbd.c" "esp_hid_gap.c")
endif()

# Generate splash image at configure time based on board selection
if(CONFIG_BOARD_P4_WAVESHARE)
    set(SPLASH_PNG "${PROJECT_DIR}/assets/blokyo-logo-720x720.png")
else()
    set(SPLASH_PNG "${PROJECT_DIR}/assets/blokyo-logo-1024x600.png")
endif()
set(SPLASH_BIN "${CMAKE_CURRENT_LIST_DIR}/splash_bgr888.bin")

if(SPLASH_PNG)
    execute_process(
        COMMAND python3 "${PROJECT_DIR}/assets/gen_splash.py" "${SPLASH_PNG}" "${SPLASH_BIN}"
        RESULT_VARIABLE SPLASH_RC
    )
    if(NOT SPLASH_RC EQUAL 0)
        message(WARNING "Failed to generate splash (rc=${SPLASH_RC}), using existing")
    endif()
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_wifi nvs_flash esp_event esp_netif
                 wolfssl wolfssh esp_lcd esp_psram
                 esp_mm espressif__esp_hosted bootloader_support
                 esp_app_format esp_partition libvterm
                 esp_lcd_st7703 bt esp_hid
    EMBED_TXTFILES "${PROJECT_DIR}/keys/id_ed25519"
                   "${PROJECT_DIR}/keys/id_ed25519.pub"
    EMBED_FILES "splash_bgr888.bin"
)
