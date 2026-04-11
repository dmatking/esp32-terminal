cmake_minimum_required(VERSION 3.16)

# Use wolfSSL user_settings.h instead of options.h (required for ESP-IDF wolfssl component)
add_compile_definitions(WOLFSSL_USER_SETTINGS)

# Pull in machine-local credentials (WiFi, etc.) without committing them
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "$ENV{HOME}/.esp_creds")
    list(APPEND SDKCONFIG_DEFAULTS "$ENV{HOME}/.esp_creds")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(terminal_p4)
