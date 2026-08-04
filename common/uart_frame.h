/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wire framing for the UART link between the PAwR central board and this
 * nRF9151 gateway. Carries a struct sensor_payload (see
 * ../../../common/pawr_protocol.h) as its body. BLE PDUs are CRC-checked at
 * the link layer, but a plain UART byte stream isn't, so this frame adds its
 * own CRC-16/CCITT-FALSE -- same algorithm choice as the prior UART_reader
 * project's AdvChannelPayload framing.
 *
 * Frame layout, all bytes on the wire in this order:
 *   [[ START_BYTE ][ struct sensor_payload, 8 bytes ][ crc16, 2 bytes LE ]]
 * Total: 11 bytes/frame. No length byte needed since the payload is a fixed
 * 8 bytes -- if pawr_protocol.h's sensor_payload ever changes size, this
 * frame format changes with it (the BUILD_ASSERT in that header will catch
 * an accidental mismatch at compile time, not here).
 */

#ifndef UART_FRAME_H_
#define UART_FRAME_H_

#include <stdint.h>
#include "pawr_protocol.h"

/* Arbitrary byte unlikely to appear as the first byte of a real frame often;
 * framing sync is start-byte + fixed-length only (no escaping) since node_id
 * (payload byte 0) is 1-17 and START_BYTE is chosen outside that range.
 */
#define UART_FRAME_START_BYTE 0xA5

struct uart_frame {
	uint8_t start;
	struct sensor_payload payload;
	uint16_t crc16;
} __packed;

#define UART_FRAME_SIZE sizeof(struct uart_frame)

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout) over
 * the raw bytes of a struct sensor_payload. Same algorithm/parameters as the
 * prior UART_reader project used for AdvChannelPayload, kept identical so
 * anyone cross-referencing the two projects isn't tripped up by a silent
 * CRC-variant mismatch.
 */
uint16_t uart_frame_crc16(const uint8_t *data, size_t len);

#endif /* UART_FRAME_H_ */
