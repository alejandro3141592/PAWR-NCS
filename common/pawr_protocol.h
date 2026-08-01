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

/* Minimal-repro test mode (2026-07-31, see NOTES.md): both sides' HCI logs
 * came back clean (central's PAST command completes status 0x00, but the
 * peripheral's controller never sees a Sync Transfer Received event at
 * all -- not even a failure), which rules out connection timing/GATT
 * ordering as the cause. This flag shrinks the timing back to the
 * original NCS periodic_adv_rsp sample's values and, in each app's main(),
 * skips the dynamic GATT slot-assignment dance (peripheral's pawr_timing
 * struct is already zero-initialized, i.e. subevent 0 / response slot 0,
 * so no peripheral code change is needed for that part). Goal: isolate
 * whether the bug is structural (present even in a near-stock config) or
 * tied to this project's larger interval/subevent count. Flip to 0 to
 * restore full dynamic-assignment production behavior -- do not leave
 * this at 1 once the experiment is done.
 */
#define APP_MINIMAL_REPRO 0

/* Scale isolation test (2026-07-31, see NOTES.md): flipping
 * APP_MINIMAL_REPRO back to 0 hit a NEW bug (unrelated to the SENDER fix) --
 * central hangs a moment after "Scanning successfully started" with
 * repeating "udc: Failed to allocate net_buf 4095, ep 0x80" and then total
 * silence. Every previously-working test today (APP_MINIMAL_REPRO=1, and the
 * literal stock sample) only ever ran at the stock sample's own light
 * defaults (5 subevents, ~319ms interval) -- full scale (20 subevents, 10s)
 * has never actually been verified to work on this hardware/SDK. This knob
 * separates the two variables stock+production conflates, to find out which
 * one (subevent count, or interval length) actually triggers the hang:
 *   0 = full production (20 subevents, 10s)      -- known broken
 *   1 = stock defaults (5 subevents, ~319ms)      -- known working
 *   2 = REMOVED, was invalid: 20 subevents needs a subevent train of
 *       20 * 40ms = 800ms, which doesn't fit inside a ~319ms periodic
 *       interval at all -- produced zero console output, but that's most
 *       likely just malformed HCI params getting rejected/hanging very
 *       early, not a real signal about subevent count alone. Don't reuse.
 *   3 = 5 subevents, 10s interval                 -- isolates INTERVAL
 *       (valid: 5 * 40ms = 200ms subevent train fits easily in either
 *       interval, so this is a clean single-variable change from mode 1)
 *       CONFIRMED WORKING over a full 30-min run both sides, 2026-08-01.
 *   4 = 10 subevents, 10s interval                 -- binary search step
 *       (valid: 10 * 40ms = 400ms subevent train, fits easily in 10s)
 * Remove this whole knob once the trigger is found and the real fix (buffer
 * pool sizing, most likely) is identified and applied instead.
 */
#define APP_SCALE_TEST 4

/* One subevent per node (17 nodes + 3 spare), one response slot per
 * subevent. interval_min/max are uint16_t in 1.25 ms units (0x1F40 * 1.25ms
 * = 10.00s exactly). subevent_interval is uint8_t in 1.25ms units,
 * response_slot_delay is uint8_t in 1.25ms units, response_slot_spacing is
 * uint8_t in 0.125ms units.
 */
#if APP_MINIMAL_REPRO
#define NUM_SUBEVENTS             5
#define PAWR_INTERVAL_UNITS       0xFF    /* ~318.75 ms, the original NCS sample's interval */
#elif APP_SCALE_TEST == 2
#define NUM_SUBEVENTS             20
#define PAWR_INTERVAL_UNITS       0xFF    /* ~318.75 ms -- count isolation */
#elif APP_SCALE_TEST == 3
#define NUM_SUBEVENTS             5
#define PAWR_INTERVAL_UNITS       0x1F40  /* 10.00 s -- interval isolation */
#elif APP_SCALE_TEST == 4
#define NUM_SUBEVENTS             10
#define PAWR_INTERVAL_UNITS       0x1F40  /* 10.00 s -- binary search step */
#else
#define NUM_SUBEVENTS             20
#define PAWR_INTERVAL_UNITS       0x1F40  /* 10.00 s */
#endif
#define NUM_RSP_SLOTS             1

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
