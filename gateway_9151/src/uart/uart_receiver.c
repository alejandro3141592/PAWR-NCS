/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "uart_frame.h"
#include "uart_receiver.h"

/* Devicetree alias "pawr-uart", provided by
 * boards/nrf9151dk_nrf9151_ns.overlay (aliases the DK's arduino_serial /
 * uart1 -- see that file for the physical pin mapping and the TF-M-default
 * override it needs on the /ns board variant).
 */
#define PAWR_UART_NODE DT_ALIAS(pawr_uart)

static const struct device *const uart_dev = DEVICE_DT_GET(PAWR_UART_NODE);

static uart_receiver_frame_cb_t frame_cb;

enum frame_state {
	FRAME_STATE_WAIT_START,
	FRAME_STATE_COLLECT,
};

static enum frame_state state = FRAME_STATE_WAIT_START;
static uint8_t frame_buf[UART_FRAME_SIZE];
static size_t frame_pos;

static void handle_byte(uint8_t byte)
{
	switch (state) {
	case FRAME_STATE_WAIT_START:
		if (byte == UART_FRAME_START_BYTE) {
			frame_buf[0] = byte;
			frame_pos = 1;
			state = FRAME_STATE_COLLECT;
		}
		break;

	case FRAME_STATE_COLLECT:
		frame_buf[frame_pos++] = byte;
		if (frame_pos == UART_FRAME_SIZE) {
			struct uart_frame frame;

			memcpy(&frame, frame_buf, UART_FRAME_SIZE);
			state = FRAME_STATE_WAIT_START;
			frame_pos = 0;

			uint16_t computed = uart_frame_crc16((const uint8_t *)&frame.payload,
							      sizeof(frame.payload));

			if (computed != frame.crc16) {
				printk("[UART] CRC mismatch, dropping frame (got 0x%04x, want 0x%04x)\n",
				       frame.crc16, computed);
				break;
			}

			if (frame_cb) {
				frame_cb(&frame.payload);
			}
		}
		break;
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t byte;

		while (uart_fifo_read(dev, &byte, 1) == 1) {
			handle_byte(byte);
		}
	}
}

int uart_receiver_init(uart_receiver_frame_cb_t cb)
{
	if (!device_is_ready(uart_dev)) {
		printk("[UART] Device not ready\n");
		return -ENODEV;
	}

	frame_cb = cb;
	state = FRAME_STATE_WAIT_START;
	frame_pos = 0;

	int err = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);

	if (err) {
		printk("[UART] Failed to set IRQ callback (err %d)\n", err);
		return err;
	}

	uart_irq_rx_enable(uart_dev);
	printk("[UART] Receiver ready\n");
	return 0;
}
