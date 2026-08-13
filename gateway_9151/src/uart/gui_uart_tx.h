/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fallback data path (2026-08-12): forwards each sensor_payload this
 * gateway receives from central out over the console UART (uart0/VCOM0,
 * the same wire printk already uses), framed per ../../../common/
 * uart_frame.h, so a PC running gui/sensor_gui.py can ingest sensor data
 * directly over USB with zero dependency on SIM/LTE/MQTT. See
 * gateway_9151/README.md and NOTES.md 2026-08-12 for why this shares the
 * console UART rather than a dedicated one (VCOM1 was deliberately
 * disabled earlier in this project to free pins for the central link, see
 * that README) and how byte-level interleaving with printk's own output on
 * this shared wire is prevented (k_sched_lock(), see gui_uart_tx.c).
 */

#ifndef GUI_UART_TX_H_
#define GUI_UART_TX_H_

#include "pawr_protocol.h"

/* Confirms the console UART device is ready. Returns 0 on success, negative
 * errno otherwise. Non-fatal to the rest of the app if this fails -- see
 * the call site in main.c.
 */
int gui_uart_tx_init(void);

/* Frames and sends payload out over the console UART, unconditionally (not
 * gated on MQTT/LTE state -- see main.c's on_uart_frame()). Blocks briefly
 * (uart_poll_out() per byte, 11 bytes total) -- called from the main
 * thread's on_uart_frame() callback, not a hot ISR path, so this is fine.
 */
void gui_uart_tx_send(const struct sensor_payload *payload);

#endif /* GUI_UART_TX_H_ */
