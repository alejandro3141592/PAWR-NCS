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

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

/* Device name prefix the peripheral advertises under and the central scans
 * for. Each peripheral appends its own CONFIG_APP_CENTRAL_ID (the central
 * it's meant to pair with, see peripheral/Kconfig) as a decimal suffix at
 * runtime -- e.g. "PAwR sync sample 3" for a peripheral targeting central
 * ID 3 -- so multiple independent central+peripheral-fleet deployments can
 * run in BLE range of each other without cross-onboarding: central only
 * scans for/connects to names starting with its own expected prefix (see
 * central's device_found()), so a peripheral flashed for central 3 is
 * invisible to central 7's onboarding loop and vice versa. See NOTES.md
 * 2026-08-05 for why this was added (observed with 2+ central/gateway rigs
 * running near each other).
 *
 * Central ID 0 is the default/"don't care" value: a central with
 * CONFIG_APP_CENTRAL_ID=0 scans for the bare, unsuffixed PAWR_ADV_NAME
 * (matching a peripheral that also has CONFIG_APP_CENTRAL_ID=0) --
 * preserves old single-central-rig behavior with no config changes needed
 * for the common case of "just one central, no need to scope anything."
 *
 * 2026-08-07: also appends "#<node_id>", e.g. "PAwR sync sample 2 #47" --
 * see pawr_format_adv_name()'s own comment for why (fixed/table-driven
 * subevent assignment needs central to know node_id before connecting).
 * Central's match check changed from an exact strcmp to a prefix check
 * accordingly (matching "PAwR sync sample 2", ignoring the "#47" that
 * varies per peripheral) -- see central's device_found().
 */
#define PAWR_ADV_NAME "PAwR sync sample"

/* Longest possible advertised name: PAWR_ADV_NAME + " " + up to 3 digits
 * (CONFIG_APP_CENTRAL_ID range) + " #" + up to 3 digits (CONFIG_APP_NODE_ID
 * range, see peripheral/Kconfig) + nul. BT_DATA_NAME_COMPLETE has no hard
 * length limit here (well under the 31-byte legacy adv payload once the
 * rest of the AD structure is accounted for), but this bounds the stack
 * buffer both apps format the name into.
 */
#define PAWR_ADV_NAME_MAX_LEN (sizeof(PAWR_ADV_NAME) + 1 + 3 + 2 + 3)

/* Formats "<PAWR_ADV_NAME>[ <central_id>][ #<node_id>]" into buf (must be
 * >= PAWR_ADV_NAME_MAX_LEN bytes). Shared by both central (what it scans
 * for/parses) and peripheral (what it advertises as) so the two can never
 * drift apart on the exact suffix format.
 *
 * node_id is embedded here (rather than fetched via a GATT read after
 * connecting) specifically so central can learn which physical node it's
 * about to onboard BEFORE connecting -- needed for fixed/table-driven
 * subevent assignment (see pawr_fixed_slot_lookup() below and NOTES.md
 * 2026-08-07): central has to pick the right slot as part of the very
 * first GATT write, and by then it's too late to still be guessing the
 * node's identity from an extra round trip. node_id == 0 omits the "#..."
 * suffix entirely, for any future caller that doesn't need per-node
 * identification at scan time (kept optional, not required, so this
 * doesn't force every use of the name-formatting helper to have a node_id
 * on hand).
 */
static inline void pawr_format_adv_name(char *buf, size_t buf_size, unsigned int central_id,
					 unsigned int node_id)
{
	if (central_id == 0 && node_id == 0) {
		snprintk(buf, buf_size, "%s", PAWR_ADV_NAME);
	} else if (node_id == 0) {
		snprintk(buf, buf_size, "%s %u", PAWR_ADV_NAME, central_id);
	} else {
		snprintk(buf, buf_size, "%s %u #%u", PAWR_ADV_NAME, central_id, node_id);
	}
}

/* Parses the node_id embedded by pawr_format_adv_name() back out of a
 * scanned advertised name, e.g. "PAwR sync sample 2 #47" -> 47. Returns 0
 * (same sentinel as "no node_id" in pawr_format_adv_name()) if name has no
 * "#<digits>" suffix, so callers can treat 0 uniformly as "unknown/not
 * present" without a separate found/not-found out-parameter.
 */
static inline unsigned int pawr_parse_node_id(const char *name)
{
	const char *hash = strchr(name, '#');

	if (!hash) {
		return 0;
	}

	return (unsigned int)strtoul(hash + 1, NULL, 10);
}

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
#define APP_SCALE_TEST 0

/* One subevent per node, one response slot per subevent. interval_min/max
 * are uint16_t in 1.25 ms units (0x1F40 * 1.25ms = 10.00s exactly).
 * subevent_interval is uint8_t in 1.25ms units, response_slot_delay is
 * uint8_t in 1.25ms units, response_slot_spacing is uint8_t in 0.125ms
 * units.
 *
 * 2026-08-07: reverted 55 -> 20 (back to a 17-node-at-once deployment
 * target). Buffer counts (central/prj.conf) reverted to 6/6 alongside this
 * -- soak-tested at 20 subevents (NOTES.md 2026-08-03, both a 30-min
 * single-node run and a 1-hour 5-node run).
 *
 * 2026-08-07 (later same day): raised 20 -> 25. Reason is capacity, not
 * concurrency -- with the new fixed/table-driven subevent assignment (see
 * central/node_slot_table.h, NOTES.md 2026-08-07), EVERY distinct node_id
 * ever assigned to a central reserves its own permanent subevent, whether
 * or not that physical board is currently powered on -- unlike the old
 * dynamic allocation, where only currently-connected peripherals consumed a
 * slot. User's real roster has up to 25 distinct node IDs on one central
 * rig (even though only ~17 run concurrently), so 20 wasn't enough table
 * capacity even though 17-concurrent would have fit fine under the old
 * model. +5 is a small, deliberate step (not the kind of large jump that
 * caused problems going 20->55 previously) -- subevent train span is still
 * comfortable (25 * 40ms = 1000ms, 10x headroom inside the 10s interval).
 * NOT yet re-validated: 6/6 buffers were only soak-tested at exactly 20
 * subevents, not 25 -- treat as the best available starting point, not a
 * proven-safe value, until a fresh soak confirms it (or finds a different
 * buffer count is needed) at 25.
 *
 * 2026-08-07 (later still): dropped 25 -> 17 for the packet-delivery-rate
 * vs. distance test (NOTES.md 2026-08-07) -- deliberately matches the real
 * 17-node deployment target's subevent-train load/timing (central still
 * has to poll all 17 every interval, same radio duty cycle as production),
 * even though only node 24 is physically powered on for this test; the
 * other 16 table entries are unpowered placeholders so central's real
 * polling overhead is representative, not an artificially light 1-node
 * test. Revert to 25 (or whatever the live roster needs) once this test is
 * done -- this is a temporary scope-down for controlled testing, not a
 * permanent capacity reduction.
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
#define NUM_SUBEVENTS             17
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
