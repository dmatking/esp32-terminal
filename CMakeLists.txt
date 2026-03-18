cmake_minimum_required(VERSION 3.16)

# Use wolfSSL user_settings.h instead of options.h (required for ESP-IDF wolfssl component)
add_compile_definitions(WOLFSSL_USER_SETTINGS)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(terminal_p4)
