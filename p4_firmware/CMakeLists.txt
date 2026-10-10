cmake_minimum_required(VERSION 3.16)

# Add local components (esp_serial_slave_link)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

# Include ESP-IDF cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(usb_bt_bridge_p4)
