/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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

	/* uart_poll_out(), not uart_fifo_fill(): this is called from
	 * response_cb, a BLE callback context, not a UART TX ISR.
	 * uart_fifo_fill()'s own doc comment states "Result of calling this
	 * function not from an ISR is undefined (hardware-dependent)" --
	 * confirmed as the actual root cause of the gateway never receiving
	 * valid frames despite a real (non-zero) voltage on the wire (see
	 * NOTES.md 2026-08-04): TX was clocking bytes out through an
	 * unsupported code path instead of failing outright, producing
	 * malformed signaling. uart_poll_out() blocks until each byte is
	 * queued and is explicitly safe to call from any context. At 11
	 * bytes/frame and up to ~20 frames per ~10s interval, the blocking
	 * cost here is negligible.
	 */
	const uint8_t *bytes = (const uint8_t *)&frame;

	for (size_t i = 0; i < sizeof(frame); i++) {
		uart_poll_out(gateway_uart_dev, bytes[i]);
	}
}
