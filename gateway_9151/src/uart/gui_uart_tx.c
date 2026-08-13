/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "gui_uart_tx.h"
#include "uart_frame.h"

/* zephyr,console in this board's own common devicetree already resolves to
 * uart0/VCOM0 (confirmed via the generated zephyr.dts) -- no new alias or
 * overlay needed, this is the same device printk's own output goes to.
 */
#define CONSOLE_UART_NODE DT_CHOSEN(zephyr_console)

static const struct device *const console_uart_dev = DEVICE_DT_GET(CONSOLE_UART_NODE);
static bool console_uart_ready;

int gui_uart_tx_init(void)
{
	if (!device_is_ready(console_uart_dev)) {
		printk("[GUI-UART] Console device not ready, fallback forwarding disabled\n");
		return -ENODEV;
	}

	console_uart_ready = true;
	printk("[GUI-UART] Fallback forwarding ready (shares console UART with this log)\n");
	return 0;
}

void gui_uart_tx_send(const struct sensor_payload *payload)
{
	if (!console_uart_ready) {
		return;
	}

	struct uart_frame frame = {
		.start = UART_FRAME_START_BYTE,
		.payload = *payload,
		.crc16 = uart_frame_crc16((const uint8_t *)payload, sizeof(*payload)),
	};
	const uint8_t *bytes = (const uint8_t *)&frame;

	/* This UART also carries printk's own text output. This project's
	 * actual build uses CONFIG_LOG_MODE_DEFERRED (confirmed in the built
	 * .config), meaning printk() output is drained by a separate
	 * "logging" kernel thread that calls uart_poll_out() byte-by-byte
	 * with no locking of its own (confirmed by reading
	 * subsys/logging/backends/log_backend_uart.c) -- so without this
	 * lock, that thread could preempt partway through the loop below and
	 * interleave log-text bytes into the middle of a frame, corrupting
	 * it (the receiver's CRC check would catch and drop it, but that's a
	 * real lost reading, not just cosmetic).
	 *
	 * k_sched_lock() prevents this thread from being preempted by any
	 * other thread (the logging thread included -- it runs at a normal,
	 * non-MetaIRQ priority) until k_sched_unlock(), without disabling
	 * interrupts. Total hold time here is at most ~11 uart_poll_out()
	 * calls, sub-millisecond at any UART baud rate this project uses --
	 * safely short for this normally-discouraged-if-held-long API. See
	 * NOTES.md 2026-08-12 for the full investigation (this is the only
	 * place in this codebase that needs this kind of lock, since
	 * gateway_uart_tx.c's UART is dedicated and not shared with logging).
	 */
	k_sched_lock();

	for (size_t i = 0; i < sizeof(frame); i++) {
		uart_poll_out(console_uart_dev, bytes[i]);
	}

	k_sched_unlock();
}
