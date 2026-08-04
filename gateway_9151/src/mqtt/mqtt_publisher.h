/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_PUBLISHER_H_
#define MQTT_PUBLISHER_H_

#include "pawr_protocol.h"

/* Blocks until LTE attach + MQTT CONNACK succeed, or returns a negative
 * errno after exhausting retries. Call once, after uart_receiver_init(),
 * before publishing anything.
 */
int mqtt_publisher_init(void);

/* Encodes payload as JSON and publishes it, one message per field (mirrors
 * the prior UART_reader project's per-field topic convention: separate
 * sensors/temperature, sensors/humidity, etc. topics rather than one
 * combined blob) -- see mqtt_publisher.c for the exact topic strings.
 * Non-blocking from the caller's perspective beyond the underlying socket
 * send; does not itself run the MQTT keepalive/poll loop (see
 * mqtt_publisher_process()).
 */
int mqtt_publisher_send(const struct sensor_payload *payload);

/* Services the underlying MQTT connection (PINGREQ keepalive, incoming
 * PUBACK etc.) -- must be called periodically from the main loop, since this
 * library uses blocking sockets driven by an explicit poll rather than its
 * own thread.
 */
void mqtt_publisher_process(void);

#endif /* MQTT_PUBLISHER_H_ */
