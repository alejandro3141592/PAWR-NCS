# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Referenced explicitly (rather than relying on boards/<board>.overlay
# filename auto-discovery, which was not picking this file up) so the skin
# temperature (MAX30205/lm75) and humidity (SHT4x) sensor nodes are always
# applied regardless of board-qualifier naming quirks.
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/xiao_ble.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pawr_peripheral)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
