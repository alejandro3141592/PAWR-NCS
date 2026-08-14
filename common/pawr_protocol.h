/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared BLE GATT protocol definitions for the central (hub) and peripheral
 * (wearable sensor node) apps. Single source of truth so the two binaries
 * can't drift apart on wire format.
 *
 * 2026-08-13: full pivot away from PAwR (Periodic Advertising with
 * Responses) to plain BLE GATT connections -- real-world testing (a person
 * wearing sensor nodes and walking around) showed PAwR sync drops
 * constantly and doesn't recover well, which is unacceptable for the actual
 * use case. New model, three phases:
 *   1. Init: central connects to each node once, writes the "start
 *      measuring" characteristic -- the node records its own t=0
 *      (k_uptime_get()) and starts its periodic sensor-read timer.
 *   2. Measurement: each node reads sensors every 10s and appends to its
 *      own on-board flash log, completely independent of any BLE
 *      connection -- no radio activity required while measuring, which is
 *      the whole point (a node walking out of range never loses anything).
 *   3. Download: after the experiment, central (operator-triggered, see
 *      central/src/main.c) connects to each node in turn and pulls its
 *      entire stored log over an indicate-based bulk-transfer
 *      characteristic.
 * See NOTES.md 2026-08-13 for the full history of what this replaces.
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
 * Name kept as "PAwR sync sample" (not renamed for the GATT pivot) so any
 * already-flashed/labeled hardware, and every existing tool/doc reference to
 * this string, keeps working -- purely a legacy string at this point, not a
 * statement about the transport.
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
 * drift apart on the exact suffix format. node_id == 0 omits the "#..."
 * suffix entirely.
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

/* Sensor payload: the one universal data unit, used for BLE GATT transfer
 * (download phase), UART framing to the gateway, and both on-board flash
 * logs (peripheral's storage_fcb_*, common/sensor_log.c). Fixed-point wire
 * format avoids float transport.
 *
 * 2026-08-13: added millis_since_init for the GATT pivot -- a rolling `seq`
 * alone was enough when data arrived live (a gap just meant "missed one"),
 * but downloaded data is a node's own complete local log with no gaps by
 * construction, so what matters now is *when* each reading happened
 * relative to that node's init-phase t=0 (see file header). No board in
 * this project has any real-time-clock/NTP source (confirmed via research
 * before this pivot), so this is deliberately relative-to-init, not wall-
 * clock time -- comparable across nodes sharing the same init moment,
 * without requiring new RTC hardware anywhere. uint32_t covers ~49 days at
 * 1ms resolution, comfortably beyond any single experiment.
 */
#define SENSOR_PAYLOAD_FLAG_TEMP_INVALID     BIT(0)
#define SENSOR_PAYLOAD_FLAG_HUMIDITY_INVALID BIT(1)

struct sensor_payload {
	uint8_t  node_id;         /* human-readable label, not used for assignment */
	uint8_t  flags;
	uint16_t seq;              /* peripheral-local rolling counter */
	int16_t  temp_cdeg;        /* skin temp, centi-degrees C (3612 = 36.12C) */
	uint16_t humidity_pct10;   /* relative humidity, tenths of a percent */
	uint32_t millis_since_init; /* ms since this node's init-phase t=0 */
} __packed;

BUILD_ASSERT(sizeof(struct sensor_payload) == 12, "sensor_payload size mismatch");

/* ======================================================
 * GATT protocol (2026-08-13, replaces the PAwR timing characteristic)
 *
 * One service, two characteristics, both under central/peripheral's
 * existing pawr_svc_uuid (kept as-is, still a private 128-bit UUID, no
 * reason to change it):
 *
 *   - "start measuring" (write-only): central writes this once per node,
 *     during the init phase. No payload needed (empty write) -- the write
 *     itself IS the signal; the peripheral's own write handler records
 *     k_uptime_get() as t0 and starts its sensor-read timer. Reuses the
 *     UUID central/peripheral already had wired up for the old timing
 *     characteristic (pawr_char_uuid) -- same characteristic slot,
 *     completely different meaning now.
 *
 *   - "download" (write request + indicate response): central writes a
 *     struct download_req to start a transfer, peripheral streams its
 *     whole flash log back as a sequence of indications framed with
 *     download_header/download_data/download_done (see below), paced by
 *     central confirming each indication before the next is sent (standard
 *     GATT indicate flow control) -- adapted from the backfill-ble-
 *     retrieval branch's already-proven request/header/data/done design,
 *     with the same-radio-collision risk that blocked it there gone
 *     entirely (there is no more concurrent PAwR subevent polling to
 *     collide with).
 * ====================================================== */

#define DOWNLOAD_MSG_HEADER 0x01
#define DOWNLOAD_MSG_DATA   0x02
#define DOWNLOAD_MSG_DONE   0x03

/* Central writes this to the download characteristic to start a transfer.
 * No parameters needed today (peripheral always sends its whole log --
 * there's no "since_seq" high-water-mark concept anymore now that live
 * reception doesn't exist, see NOTES.md 2026-08-13) -- kept as a distinct,
 * empty-bodied struct rather than an empty write so the wire protocol has
 * an explicit, self-documenting "this is a download request" moment, and
 * so a future resume/since-timestamp parameter (see NOTES.md open
 * question) has an obvious place to go without changing the write's shape.
 */
struct download_req {
	uint8_t reserved;
} __packed;

/* Every indication on the download characteristic starts with this msg_type
 * byte so central can tell header/data/done apart without a separate
 * out-of-band state machine.
 */
struct download_header {
	uint8_t  msg_type;       /* DOWNLOAD_MSG_HEADER */
	uint16_t total_entries;
} __packed;

/* Flexible-array payload, one or more sensor_payload entries per
 * indication -- central computes how many fit per PDU from the negotiated
 * ATT MTU (see bt_gatt_get_mtu()), same approach already proven on the
 * backfill-ble-retrieval branch.
 */
struct download_data_pdu {
	uint8_t msg_type;                  /* DOWNLOAD_MSG_DATA */
	uint8_t count;
	struct sensor_payload entries[];
} __packed;

struct download_done {
	uint8_t  msg_type;      /* DOWNLOAD_MSG_DONE */
	uint16_t entries_sent;
} __packed;

#endif /* PAWR_PROTOCOL_H_ */
