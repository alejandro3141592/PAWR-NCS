/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal operator-input mechanism over central's USB console (2026-08-13,
 * see common/pawr_protocol.h's file header for the GATT pivot this
 * supports): watches the console UART's RX for single trigger bytes so the
 * operator can switch central between INIT and DOWNLOAD mode without a
 * reflash. Deliberately NOT Zephyr's shell subsystem (CONFIG_SHELL) --
 * this project has repeatedly kept its own minimal, purpose-built code over
 * pulling in heavier subsystems (see e.g. common/uart_frame.h's own framing
 * instead of a generic protocol), and CONFIG_SHELL specifically hung this
 * project's console once before on the peripheral side (see NOTES.md
 * 2026-08-03) -- reason enough to avoid it here too, on the one board
 * that's actually meant to stay reachable/interactive throughout an
 * experiment.
 */

#ifndef CONSOLE_TRIGGER_H_
#define CONSOLE_TRIGGER_H_

#include <zephyr/kernel.h>

/* Called from UART ISR context for every byte received on the console --
 * keep this fast, no blocking calls (matches the ISR-callback pattern
 * already used by gateway_9151/src/uart/uart_receiver.c).
 */
typedef void (*console_trigger_byte_cb_t)(uint8_t byte);

/* Registers cb against the console UART's RX interrupt and enables RX.
 * Returns 0 on success, negative errno otherwise -- non-fatal to the rest
 * of the app if this fails (the operator just loses the ability to switch
 * modes at runtime; everything else keeps working).
 */
int console_trigger_init(console_trigger_byte_cb_t cb);

#endif /* CONSOLE_TRIGGER_H_ */
