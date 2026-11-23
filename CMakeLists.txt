cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32c3" CACHE STRING "Target chip")
set(PROJECT_VER "0.1.0")
set(BRIDGE_EXPECT_INTERNAL_LOOPBACK OFF CACHE BOOL "Require the explicit diagnostic mode")
set(BRIDGE_EXPECT_LUATOS OFF CACHE BOOL "Require the LuatOS CH343 board profile")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c3_usb_uart)

if(NOT IDF_TARGET STREQUAL "esp32c3")
    message(FATAL_ERROR "This board pin assignment supports only ESP32-C3")
endif()

if(BRIDGE_EXPECT_INTERNAL_LOOPBACK AND NOT CONFIG_BRIDGE_INTERNAL_LOOPBACK)
    message(FATAL_ERROR "Diagnostic build requested, but internal loopback is disabled")
elseif(NOT BRIDGE_EXPECT_INTERNAL_LOOPBACK AND CONFIG_BRIDGE_INTERNAL_LOOPBACK)
    message(FATAL_ERROR "Normal build must not contain the internal-loopback diagnostic")
endif()

if(BRIDGE_EXPECT_LUATOS AND NOT CONFIG_BRIDGE_LUATOS)
    message(FATAL_ERROR "LuatOS build requested, but the CH343 profile is disabled")
elseif(NOT BRIDGE_EXPECT_LUATOS AND CONFIG_BRIDGE_LUATOS)
    message(FATAL_ERROR "Native USB build must not contain the CH343 profile")
endif()
