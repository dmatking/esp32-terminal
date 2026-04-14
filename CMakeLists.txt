cmake_minimum_required(VERSION 3.16)

# Pull in machine-local credentials (WiFi, etc.) without committing them
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "$ENV{HOME}/.esp_creds")
    list(APPEND SDKCONFIG_DEFAULTS "$ENV{HOME}/.esp_creds")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(terminal_p4)
