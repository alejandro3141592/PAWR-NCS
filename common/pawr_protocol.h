/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared PAwR timing/payload definitions for the central (advertiser) and
 * peripheral (sync/responder) apps. Single source of truth so the two
 * binaries can't drift apart on subevent/slot layout or wire format.
 */

#ifndef PAWR_PROTOCOL_H_
#define PAWR_PROTOCOL_H_

#include <zephyr/sys/util.h>

/* Device name the peripheral advertises as and the central scans for.
 * Kconfig can't include this header, so peripheral/prj.conf's
 * CONFIG_BT_DEVICE_NAME must be kept equal to this literal by hand.
 */
#define PAWR_ADV_NAME "PAwR sync sample"

/* One subevent per node (17 nodes + 3 spare), one response slot per
 * subevent. interval_min/max are uint16_t in 1.25 ms units (0x1F40 * 1.25ms
 * = 10.00s exactly). subevent_interval is uint8_t in 1.25ms units,
 * response_slot_delay is uint8_t in 1.25ms units, response_slot_spacing is
 * uint8_t in 0.125ms units.
 */
#define NUM_SUBEVENTS             20
#define NUM_RSP_SLOTS             1

#define PAWR_INTERVAL_UNITS       0x1F40  /* 10.00 s */
#define PAWR_SUBEVENT_INTERVAL    0x20    /* 40 ms   */
#define PAWR_RESPONSE_SLOT_DELAY  0x8     /* 10 ms   */
#define PAWR_RESPONSE_SLOT_SPACING 0x50   /* 10 ms   */

/* PAWR_INTERVAL_UNITS converted to real milliseconds (units are 1.25ms
 * each): 8000 * 1.25 = 10000ms = 10.00s. Both apps use this directly instead
 * of repeating the unit conversion inline.
 */
#define PAWR_INTERVAL_MS          (PAWR_INTERVAL_UNITS * 5 / 4)

/* PAST subscribe timeout on the peripheral: 10ms units, 30s = 3 missed
 * 10s intervals of margin before sync is torn down.
 */
#define PAWR_PAST_TIMEOUT_UNITS   3000

/* A subevent with no response seen for this many missed intervals is
 * considered abandoned and eligible for reassignment by central.
 */
#define PAWR_SLOT_STALE_INTERVALS 3

/* Sensor response payload (peripheral -> central), sent as
 * BT_DATA_MANUFACTURER_DATA. Fixed-point wire format avoids float transport.
 */
#define SENSOR_PAYLOAD_FLAG_TEMP_INVALID     BIT(0)
#define SENSOR_PAYLOAD_FLAG_HUMIDITY_INVALID BIT(1)

struct sensor_payload {
	uint8_t  node_id;         /* human-readable label, not used for assignment */
	uint8_t  flags;
	uint16_t seq;              /* peripheral-local rolling counter */
	int16_t  temp_cdeg;        /* skin temp, centi-degrees C (3612 = 36.12C) */
	uint16_t humidity_pct10;   /* relative humidity, tenths of a percent */
} __packed;

BUILD_ASSERT(sizeof(struct sensor_payload) == 8, "sensor_payload size mismatch");

#endif /* PAWR_PROTOCOL_H_ */
