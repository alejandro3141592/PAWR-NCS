/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UART_RECEIVER_H_
#define UART_RECEIVER_H_

#include "pawr_protocol.h"

/* Called from the UART ISR/work context whenever a complete frame passes
 * CRC validation. Keep this fast -- no blocking calls (matches the
 * ISR-ring-buffer + framing pattern from the prior UART_reader project's
 * uart_receiver.c). mqtt_publisher owns queuing the actual publish so this
 * callback can return quickly.
 */
typedef void (*uart_receiver_frame_cb_t)(const struct sensor_payload *payload);

/* Initializes the UART device (from devicetree, see the boards folder),
 * registers the interrupt callback, and starts RX. Returns 0 on success,
 * negative errno otherwise (mirrors Zephyr driver API convention).
 */
int uart_receiver_init(uart_receiver_frame_cb_t cb);

#endif /* UART_RECEIVER_H_ */
