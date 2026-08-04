# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pawr_gateway_9151)

target_sources(app PRIVATE
    src/main.c
    ../common/uart_frame.c
    ../common/sensor_log.c
    src/uart/uart_receiver.c
    src/mqtt/mqtt_publisher.c
)
target_sources_ifdef(CONFIG_APP_MQTT_USE_TLS app PRIVATE src/mqtt/tls_provision.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
