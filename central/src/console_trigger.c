/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include "console_trigger.h"

/* zephyr,console already resolves to this board's own USB-CDC-ACM console
 * device (confirmed via the generated devicetree) -- the same physical
 * connection printk() output already uses, so no new wiring/hardware.
 */
#define CONSOLE_UART_NODE DT_CHOSEN(zephyr_console)

static const struct device *const console_uart_dev = DEVICE_DT_GET(CONSOLE_UART_NODE);

static void console_uart_isr(const struct device *dev, void *user_data)
{
	console_trigger_byte_cb_t cb = user_data;
	uint8_t byte;

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		while (uart_fifo_read(dev, &byte, 1) == 1) {
			cb(byte);
		}
	}
}

int console_trigger_init(console_trigger_byte_cb_t cb)
{
	if (!device_is_ready(console_uart_dev)) {
		printk("[CONSOLE] Console device not ready, operator trigger disabled\n");
		return -ENODEV;
	}

	int err = uart_irq_callback_user_data_set(console_uart_dev, console_uart_isr, cb);

	if (err) {
		printk("[CONSOLE] Failed to set IRQ callback (err %d)\n", err);
		return err;
	}

	uart_irq_rx_enable(console_uart_dev);

	return 0;
}
