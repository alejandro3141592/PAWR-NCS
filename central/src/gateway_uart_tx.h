/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Forwards each sensor_payload central receives from a peripheral out over
 * uart1 (see boards/xiao_ble_nrf52840.overlay) to the nRF9151 gateway board,
 * framed per ../common/uart_frame.h.
 */

#ifndef GATEWAY_UART_TX_H_
#define GATEWAY_UART_TX_H_

#include "pawr_protocol.h"

/* Initializes uart1. Returns 0 on success, negative errno otherwise. Safe to
 * call even if the gateway board isn't connected yet/at all -- TX just goes
 * nowhere in that case, this never blocks waiting for a peer.
 */
int gateway_uart_tx_init(void);

/* Frames and sends payload out over uart1. Called from response_cb, a hot
 * path (up to NUM_SUBEVENTS times per PAWR interval) -- keep this cheap;
 * it fills the UART's TX FIFO and returns, it does not block on the byte
 * actually leaving the wire.
 */
void gateway_uart_tx_send(const struct sensor_payload *payload);

#endif /* GATEWAY_UART_TX_H_ */
