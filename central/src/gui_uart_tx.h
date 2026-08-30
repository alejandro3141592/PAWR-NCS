/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Direct-to-PC fallback (2026-08-30): forwards each sensor_payload central
 * receives out over its own USB console (the same CDC-ACM device printk's
 * output goes to), framed per ../../common/uart_frame.h -- identical wire
 * format to gateway_9151/src/uart/gui_uart_tx.c, so gui/sensor_gui.py's
 * existing UARTWorker can read a central directly with zero code changes on
 * the GUI side. Added for a rig whose gateway_9151 board's USB port had
 * failed: central was still receiving PAwR data fine, but nothing reached
 * the PC because gateway_9151 -> GUI was the only path that existed. This
 * is a second, independent path (not a replacement) -- central still also
 * forwards to gateway_9151 over uart1 via gateway_uart_tx.c, unaffected by
 * this addition.
 */

#ifndef GUI_UART_TX_H_
#define GUI_UART_TX_H_

#include "pawr_protocol.h"

/* Confirms the console UART device is ready. Returns 0 on success, negative
 * errno otherwise. Non-fatal to the rest of the app if this fails -- see
 * the call site in main.c.
 */
int gui_uart_tx_init(void);

/* Frames and sends payload out over the console UART, unconditionally.
 * Called from response_cb, the same hot path gateway_uart_tx_send() is
 * called from -- see that function's own comment on why uart_poll_out() is
 * required here (not uart_fifo_fill()) and gui_uart_tx.c's k_sched_lock()
 * comment for why this one (unlike gateway_uart_tx_send()) needs to guard
 * against interleaving with this device's own printk/logging output.
 */
void gui_uart_tx_send(const struct sensor_payload *payload);

#endif /* GUI_UART_TX_H_ */
