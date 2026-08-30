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

/* zephyr,console on this board resolves to the USB CDC-ACM device (see
 * boards/xiao_ble_nrf52840.overlay's own comment: "this board's console,
 * routed to USB-CDC-ACM; zephyr,console = &board_cdc_acm_uart") -- the same
 * device printk's own output goes to, and the same one every capture tool
 * used throughout this project (capture_flash_dump.py, Watch-SerialLog.ps1)
 * already reads from.
 */
#define CONSOLE_UART_NODE DT_CHOSEN(zephyr_console)

static const struct device *const console_uart_dev = DEVICE_DT_GET(CONSOLE_UART_NODE);
static bool console_uart_ready;

int gui_uart_tx_init(void)
{
	if (!device_is_ready(console_uart_dev)) {
		printk("[GUI-UART] Console device not ready, direct-to-PC forwarding disabled\n");
		return -ENODEV;
	}

	console_uart_ready = true;
	printk("[GUI-UART] Direct-to-PC forwarding ready (shares console UART with this log)\n");
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

	/* This UART also carries printk's own text output, drained by a
	 * separate logging thread with no locking of its own (CONFIG_LOG_MODE_
	 * DEFERRED=y, confirmed in the built .config) -- without this lock,
	 * that thread could preempt partway through the loop below and
	 * interleave log-text bytes into the middle of a frame, corrupting it
	 * (the GUI's CRC check would catch and drop it, but that's a real lost
	 * reading, not just cosmetic). Identical reasoning/fix as
	 * gateway_9151/src/uart/gui_uart_tx.c's own k_sched_lock() -- see that
	 * file's comment for the full investigation. gateway_uart_tx.c doesn't
	 * need this because uart1 there is a dedicated wire, not shared with
	 * logging.
	 */
	k_sched_lock();

	for (size_t i = 0; i < sizeof(frame); i++) {
		uart_poll_out(console_uart_dev, bytes[i]);
	}

	k_sched_unlock();
}
