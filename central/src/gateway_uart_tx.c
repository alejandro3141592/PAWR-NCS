/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include "gateway_uart_tx.h"
#include "uart_frame.h"

#define GATEWAY_UART_NODE DT_ALIAS(gateway_uart)

static const struct device *const gateway_uart_dev = DEVICE_DT_GET(GATEWAY_UART_NODE);
static bool gateway_uart_ready;

int gateway_uart_tx_init(void)
{
	if (!device_is_ready(gateway_uart_dev)) {
		printk("[GW-UART] Device not ready, sensor data will not be forwarded\n");
		return -ENODEV;
	}

	gateway_uart_ready = true;
	printk("[GW-UART] Ready\n");
	return 0;
}

void gateway_uart_tx_send(const struct sensor_payload *payload)
{
	if (!gateway_uart_ready) {
		return;
	}

	struct uart_frame frame = {
		.start = UART_FRAME_START_BYTE,
		.payload = *payload,
		.crc16 = uart_frame_crc16((const uint8_t *)payload, sizeof(*payload)),
	};

	const uint8_t *bytes = (const uint8_t *)&frame;
	size_t remaining = sizeof(frame);

	/* uart_fifo_fill may accept fewer bytes than requested if the FIFO is
	 * momentarily full; loop until the whole (tiny, 11-byte) frame is
	 * queued. This still doesn't block on the bytes actually clearing the
	 * wire, only on FIFO space, which drains at the UART's baud rate
	 * (~1ms for 11 bytes at 115200) -- negligible next to the ~500ms
	 * worst-case gap between subevents.
	 */
	while (remaining > 0) {
		int sent = uart_fifo_fill(gateway_uart_dev, bytes, remaining);

		if (sent <= 0) {
			break;
		}
		bytes += sent;
		remaining -= sent;
	}
}
