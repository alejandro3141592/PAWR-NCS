/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal console-only test: strips out everything gateway_9151 normally
 * does (modem, MQTT, UART receiver, flash log) down to just booting and
 * printing, to isolate whether gateway_9151's "zero serial output despite
 * a confirmed-healthy CPU" mystery (see NOTES.md 2026-08-04) is caused by
 * something in that app, or is a board/console-level problem independent
 * of any application code. Toggles LED1 every second as a visual heartbeat
 * that doesn't depend on the serial link at all -- if the LED blinks but
 * nothing appears on the console, that's strong evidence the app is fine
 * and the fault is specifically in the console UART/VCOM0 path.
 *
 * Also writes directly to uart2 (P0.23 TX / P0.24 RX, see
 * boards/nrf9151dk_nrf9151_ns.overlay) via uart_poll_out() -- deliberately
 * bypassing printk()/the console subsystem entirely, since uart0's own
 * console path is the thing under suspicion. uart2 isn't wired to the DK's
 * Interface MCU/USB at all, so seeing output here requires an external
 * USB-to-serial adapter, but it proves/disproves whether the SiP's UART
 * hardware itself is healthy, independent of the IMCU/VCOM path.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(console_test, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, { 0 });
static const struct device *const diag_uart = DEVICE_DT_GET_OR_NULL(DT_ALIAS(diag_uart));

static void diag_uart_print(const char *s)
{
	if (!diag_uart) {
		return;
	}

	for (; *s; s++) {
		uart_poll_out(diag_uart, *s);
	}
}

int main(void)
{
	printk("[CONSOLE_TEST] printk: booted\n");
	LOG_INF("LOG_INF: booted");

	bool led_ok = gpio_is_ready_dt(&led);

	if (led_ok) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	} else {
		printk("[CONSOLE_TEST] LED not ready, heartbeat will be console-only\n");
	}

	bool diag_uart_ok = diag_uart && device_is_ready(diag_uart);

	if (diag_uart_ok) {
		diag_uart_print("[DIAG_UART] uart2 (P0.23/P0.24) ready, booted\r\n");
	} else {
		printk("[CONSOLE_TEST] diag_uart (uart2) not ready\n");
	}

	uint32_t counter = 0;
	char line[48];

	while (1) {
		printk("[CONSOLE_TEST] printk tick %u\n", counter);
		LOG_INF("LOG_INF tick %u", counter);

		if (diag_uart_ok) {
			snprintk(line, sizeof(line), "[DIAG_UART] tick %u\r\n", counter);
			diag_uart_print(line);
		}

		if (led_ok) {
			gpio_pin_toggle_dt(&led);
		}

		counter++;
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
