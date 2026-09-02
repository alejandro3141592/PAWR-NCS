/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PAwR central / advertiser: skin sensor hub.
 * Adapted from NCS sample: samples/bluetooth/periodic_adv_rsp
 *
 * Flow: advertise + periodic-advertise -> scan for peripherals named
 * "PAwR sync sample" -> connect -> transfer periodic sync info (PAST) ->
 * discover the peripheral's GATT characteristic -> write it a free
 * subevent/response-slot assignment -> disconnect -> repeat forever, while
 * every subevent poll is answered (or not) by synced peripherals with their
 * latest skin temperature + humidity reading, which is parsed and printed.
 *
 * Assignment is static/fixed (2026-08-07, see node_slot_table.h and this
 * file's own "Fixed slot assignment" comment below): a compile-time
 * node_id -> subevent lookup, not handed out dynamically at onboarding time
 * as an earlier version of this file did -- that scheme was replaced to
 * avoid the silent-collision risk of a formula over a sparse,
 * non-contiguous set of real node IDs. The GATT write used here still
 * carries the (now-fixed) assignment to the peripheral, the same mechanism
 * a future dynamic scheduler would reuse if one is added back.
 */

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <bluetooth/hci_vs_sdc.h>

#include "pawr_protocol.h"
#include "gateway_uart_tx.h"
#include "gui_uart_tx.h"
#include "sensor_log.h"
#include "node_slot_table.h"
#include "watchdog.h"

/* Retired experiment (2026-08-03, see NOTES.md): stopping periodic
 * advertising during the onboarding connect step did eliminate the 0x08
 * CONN_TIMEOUTs (confirmed the radio-contention theory), but broke PAST
 * itself -- bt_le_per_adv_set_info_transfer() needs the periodic advertising
 * set actually running to have anything to transfer sync info about, and it
 * was stopped at exactly the moment PAST gets sent. Replaced by two
 * non-disruptive mitigations below: sdc_hci_cmd_vs_allow_parallel_connection_
 * establishments (a real SDC feature specifically for "initiator + PAwR
 * advertiser at the same time") and a connection interval that's a clean
 * multiple of PAWR_SUBEVENT_INTERVAL so the two schedules land on
 * predictable boundaries instead of colliding unpredictably.
 */
#define APP_STOP_PAWR_DURING_ONBOARDING 0

#define PACKET_SIZE 5
#define NAME_LEN    30

/* This central only onboards peripherals advertising this exact name --
 * see pawr_format_adv_name() in common/pawr_protocol.h and
 * CONFIG_APP_CENTRAL_ID in Kconfig for why/format. Built once at startup
 * (see main()), not per scan callback.
 */
static char target_adv_name[PAWR_ADV_NAME_MAX_LEN];

static const struct gpio_dt_spec tx_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_discovered, 0, 1);
static K_SEM_DEFINE(sem_written, 0, 1);
static K_SEM_DEFINE(sem_disconnected, 0, 1);

struct k_poll_event events[] = {
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					&sem_connected, 0),
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
					&sem_disconnected, 0),
};

static struct bt_uuid_128 pawr_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));
static uint16_t pawr_attr_handle;
static const struct bt_le_per_adv_param per_adv_params = {
	.interval_min = PAWR_INTERVAL_UNITS,
	.interval_max = PAWR_INTERVAL_UNITS,
	.options = 0,
	.num_subevents = NUM_SUBEVENTS,
	.subevent_interval = PAWR_SUBEVENT_INTERVAL,
	.response_slot_delay = PAWR_RESPONSE_SLOT_DELAY,
	.response_slot_spacing = PAWR_RESPONSE_SLOT_SPACING,
	.num_response_slots = NUM_RSP_SLOTS,
};

static struct bt_le_per_adv_subevent_data_params subevent_data_params[NUM_SUBEVENTS];
static struct net_buf_simple bufs[NUM_SUBEVENTS];
static uint8_t backing_store[NUM_SUBEVENTS][PACKET_SIZE];

BUILD_ASSERT(ARRAY_SIZE(bufs) == ARRAY_SIZE(subevent_data_params));
BUILD_ASSERT(ARRAY_SIZE(backing_store) == ARRAY_SIZE(subevent_data_params));

static uint8_t counter;

/* ======================================================
 * Fixed slot assignment: node_id -> subevent is a permanent lookup in
 * node_slot_table.h, not dynamically handed out at onboarding time (see
 * NOTES.md 2026-08-07 for why -- avoids the silent-collision risk of a
 * formula like node_id % NUM_SUBEVENTS over a sparse, non-contiguous set of
 * ~50 real node IDs). No staleness/reclaim bookkeeping is needed anymore:
 * a node's slot never changes and is never handed to anyone else, so
 * there's nothing to go stale or leak.
 * ====================================================== */

/* Fails fast (rather than silently misbehaving at runtime) if the table has
 * two entries for the same central_id claiming the same subevent -- the
 * exact hazard a fixed table exists to prevent. O(n^2) in table size, but
 * this runs once at boot against a list sized in the tens, not a hot path.
 */
static void node_slot_table_validate(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(node_slot_table); i++) {
		if (node_slot_table[i].subevent >= NUM_PRIMARY_SLOTS) {
			printk("FATAL: node_slot_table[%d] (node %u) has subevent %u >= NUM_PRIMARY_SLOTS (%d) -- primary slots must stay in the first block, redundant copies are derived by adding k * NUM_PRIMARY_SLOTS\n",
			       (int)i, node_slot_table[i].node_id, node_slot_table[i].subevent,
			       NUM_PRIMARY_SLOTS);
			k_panic();
		}

		for (unsigned int k = 0; k < NUM_REDUNDANT_COPIES; k++) {
			uint32_t copy = (uint32_t)node_slot_table[i].subevent + k * NUM_PRIMARY_SLOTS;

			if (copy >= NUM_SUBEVENTS) {
				printk("FATAL: node_slot_table[%d] (node %u) has redundant copy %u subevent %u >= NUM_SUBEVENTS (%d)\n",
				       (int)i, node_slot_table[i].node_id, k, copy, NUM_SUBEVENTS);
				k_panic();
			}
		}

		for (size_t j = i + 1; j < ARRAY_SIZE(node_slot_table); j++) {
			if (node_slot_table[i].central_id == node_slot_table[j].central_id &&
			    node_slot_table[i].subevent == node_slot_table[j].subevent) {
				printk("FATAL: node_slot_table has a collision on central %u subevent %u -- nodes %u and %u\n",
				       node_slot_table[i].central_id, node_slot_table[i].subevent,
				       node_slot_table[i].node_id, node_slot_table[j].node_id);
				k_panic();
			}

			if (node_slot_table[i].central_id == node_slot_table[j].central_id &&
			    node_slot_table[i].node_id == node_slot_table[j].node_id) {
				printk("FATAL: node_slot_table has node %u listed twice for central %u\n",
				       node_slot_table[i].node_id, node_slot_table[i].central_id);
				k_panic();
			}
		}
	}
}

#if CONFIG_APP_ACCEPT_ANY_NODE
/* Bring-up/triage mode only (see Kconfig) -- hands out subevents 0, 1, 2...
 * to whichever peripherals show up, in order, no reclaim/reuse. Only ever
 * used to test one physical node at a time (reflash between nodes), so
 * running out of subevents mid-session isn't a real concern in practice.
 */
static unsigned int next_test_slot;
#endif

/* Looks up the fixed subevent for (CONFIG_APP_CENTRAL_ID, node_id). Returns
 * -1 if this node_id has no entry for this central -- caller must refuse to
 * onboard it rather than falling back to any kind of dynamic assignment
 * (see device_found()): an unlisted node staying dark until it's added to
 * the table is the intended failure mode, not a bug to work around.
 */
static int lookup_fixed_slot(unsigned int node_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(node_slot_table); i++) {
		if (node_slot_table[i].central_id == CONFIG_APP_CENTRAL_ID &&
		    node_slot_table[i].node_id == node_id) {
			return node_slot_table[i].subevent;
		}
	}

	return -1;
}

static void request_cb(struct bt_le_ext_adv *adv, const struct bt_le_per_adv_data_request *request)
{
	int err;
	uint8_t to_send;
	struct net_buf_simple *buf;

	to_send = MIN(request->count, ARRAY_SIZE(subevent_data_params));

	for (size_t i = 0; i < to_send; i++) {
		buf = &bufs[i];
		buf->data[buf->len - 1] = counter++;

		subevent_data_params[i].subevent =
			(request->start + i) % per_adv_params.num_subevents;
		subevent_data_params[i].response_slot_start = 0;
		subevent_data_params[i].response_slot_count = NUM_RSP_SLOTS;
		subevent_data_params[i].data = buf;
	}

	err = bt_le_per_adv_set_subevent_data(adv, to_send, subevent_data_params);
	if (err) {
		printk("Failed to set subevent data (err %d)\n", err);
	} else {
		gpio_pin_toggle_dt(&tx_led);
	}
}

static struct bt_conn *default_conn;
/* True only while a bt_conn_le_create() call in device_found() has actually
 * succeeded and the resulting connection hasn't been resolved yet (either
 * connected_cb/disconnected_cb fired, or the connect_wait_timeout_ms wait
 * below gave up on it). main()'s loop must only enter its post-connect
 * k_poll() wait while this is true -- that wait is meaningless, and
 * default_conn may be NULL, on every iteration where scanning simply found
 * no matching peripheral (the normal steady state once all nodes are synced,
 * or whenever every peripheral is briefly out of range) or where
 * connected_cb's own error path (e.g. the controller's 3s
 * CONFIG_BT_CREATE_CONN_TIMEOUT firing before this loop's own much longer
 * bound) already cleared default_conn out from under it. Confirmed on real
 * hardware 2026-08-31: without this gate, the unconditional k_poll() call
 * waits out its own full timeout for a connection that was never attempted
 * this iteration, then dereferences a NULL default_conn inside
 * bt_conn_disconnect() when it gives up -- a hard fault, not just a stall,
 * and one that fires on essentially every idle scan cycle since "no
 * peripheral found this iteration" is the common case, not the exception.
 */
static bool connect_pending;
/* Fixed subevent for whichever peripheral device_found() just decided to
 * connect to -- resolved from node_slot_table.h before the connection is
 * created, carried across to the GATT write later in the onboarding
 * sequence. Only one onboarding attempt is ever in flight at a time (see
 * default_conn's own single-connection design, central/src/main.c's file
 * header), so a single static variable is safe here -- no risk of two
 * concurrent onboardings overwriting each other's pending_slot.
 */
static int pending_slot;
#if APP_STOP_PAWR_DURING_ONBOARDING
static struct bt_le_ext_adv *pawr_adv;
#endif

/* Redundant-slot dedup (see common/pawr_protocol.h's NUM_PRIMARY_SLOTS
 * comment): each node answers on two subevents (primary + backup) with the
 * SAME seq every interval, so both can legitimately succeed -- without this,
 * every reading would get forwarded to the gateway/DB twice on a good
 * interval, not just once on a lucky recovery. Indexed directly by
 * sensor_payload.node_id (uint8_t, so this covers the full possible range
 * regardless of the current CONFIG_APP_NODE_ID Kconfig limit). -1
 * (impossible for a uint16_t wire seq) means "nothing forwarded yet for
 * this node_id".
 */
static int32_t last_forwarded_seq[UINT8_MAX + 1];

static void response_cb(struct bt_le_ext_adv *adv, struct bt_le_per_adv_response_info *info,
		     struct net_buf_simple *buf)
{
	if (!buf || buf->len < 3) {
		return;
	}

	/* Manufacturer-specific data AD element: len, type, company_id(2),
	 * then the sensor_payload bytes.
	 */
	uint8_t ad_len = net_buf_simple_pull_u8(buf);
	uint8_t ad_type = net_buf_simple_pull_u8(buf);

	if (ad_type != BT_DATA_MANUFACTURER_DATA || ad_len < 1 + 2 + sizeof(struct sensor_payload)) {
		return;
	}

	(void)net_buf_simple_pull_le16(buf); /* company ID, not needed here */

	if (buf->len < sizeof(struct sensor_payload)) {
		return;
	}

	struct sensor_payload payload;

	memcpy(&payload, buf->data, sizeof(payload));

	/* Redundant-slot dedup: this exact seq for this node_id may already
	 * have been forwarded via its OTHER subevent (primary vs. backup)
	 * earlier in the same interval -- see last_forwarded_seq's own
	 * comment. Still counted/printed below so the console/log shows both
	 * receptions for diagnostics; only the gateway/on-board-flash forward
	 * is skipped for the duplicate.
	 */
	bool is_duplicate = (last_forwarded_seq[payload.node_id] == (int32_t)payload.seq);

	if (!is_duplicate) {
		last_forwarded_seq[payload.node_id] = (int32_t)payload.seq;
		gateway_uart_tx_send(&payload);
		gui_uart_tx_send(&payload);
		sensor_log_append(&payload);
	}

	/* Single printk call instead of up to 4 -- this callback fires once
	 * per received response, per subevent, per interval (up to
	 * NUM_SUBEVENTS times every PAWR_INTERVAL_MS), so at higher subevent
	 * counts it's a genuinely hot path competing for the same USB
	 * CDC-ACM console transport that's also seeing "udc: Failed to
	 * allocate net_buf" under load -- see NOTES.md 2026-08-03. Fewer,
	 * larger writes reduce that pressure vs. many small ones.
	 */
	printk(">>> Node %02u (subevent %d): skin_temp=%d.%02uC humidity=%u.%u%% seq=%u rssi=%ddBm%s%s%s\n",
	       payload.node_id, info->subevent,
	       payload.temp_cdeg / 100, abs(payload.temp_cdeg % 100),
	       payload.humidity_pct10 / 10, payload.humidity_pct10 % 10, payload.seq, info->rssi,
	       (payload.flags & SENSOR_PAYLOAD_FLAG_TEMP_INVALID) ? "  [FLAG: TEMP_FAIL]" : "",
	       (payload.flags & SENSOR_PAYLOAD_FLAG_HUMIDITY_INVALID) ? "  [FLAG: HUMIDITY_FAIL]" : "",
	       is_duplicate ? "  [DUP: backup slot, already forwarded]" : "");
}

static const struct bt_le_ext_adv_cb adv_cb = {
	.pawr_data_request = request_cb,
	.pawr_response = response_cb,
};

void connected_cb(struct bt_conn *conn, uint8_t err)
{
	printk("Connected (err 0x%02X)\n", err);

	__ASSERT(conn == default_conn, "Unexpected connected callback");

	if (err) {
		bt_conn_unref(default_conn);
		default_conn = NULL;

		/* Without this, main()'s k_poll() has nothing to wake it until
		 * its own connect_wait_timeout_ms (25s) bound expires, even
		 * though the real failure (most commonly the controller's own
		 * CONFIG_BT_CREATE_CONN_TIMEOUT, 3s by default and unoverridden
		 * in this build) is already known right here, ~22s earlier.
		 * connect_pending intentionally stays true across this call --
		 * it still tracks "an attempt was made and isn't resolved yet"
		 * correctly, since this give() is what resolves it, mirroring
		 * disconnected_cb below; main()'s own K_NO_WAIT sem_connected
		 * check right after k_poll() returns already handles telling
		 * this apart from a real successful connection.
		 */
		k_sem_give(&sem_disconnected);
	}
}

void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected, reason 0x%02X %s\n", reason, bt_hci_err_to_str(reason));

	k_sem_give(&sem_disconnected);
}

void remote_info_available_cb(struct bt_conn *conn, struct bt_conn_remote_info *remote_info)
{
	/* Need to wait for remote info before initiating PAST */
	k_sem_give(&sem_connected);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.remote_info_available = remote_info_available_cb,
};

static bool data_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;
	uint8_t len;

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		len = MIN(data->data_len, NAME_LEN - 1);
		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	default:
		return true;
	}
}

/* Short interval / long supervision timeout (10s), the opposite direction
 * from an earlier attempt at 100-150ms. Per the SoftDevice Controller's
 * scheduling docs, periodic advertising is scheduled like a central-role
 * timing-activity that can collide with this GATT connection, and a *slow*
 * connection interval gives fewer chances per second to recover from a
 * dropped connection event -- a single collision with the ~800ms
 * subevent-train burst (20 subevents x 40ms) could eat 5-8+ consecutive
 * connection events at a 100-150ms interval, most of the way to a
 * supervision timeout on its own. A short interval gives many more retry
 * opportunities in the same window, while the long timeout still tolerates
 * the occasional dropped event without tearing down the link.
 *
 * 2026-08-03: changed from 0x0C (15ms) to 0x20 (40ms) -- a clean multiple
 * of PAWR_SUBEVENT_INTERVAL (also 40ms) -- per Nordic's own scheduling
 * guidance to give colliding roles a common factor in their intervals so
 * they land on predictable, repeating boundaries instead of drifting past
 * each other unpredictably. Combined with
 * hci_vs_sdc_allow_parallel_connection_establishments (see main(), enabled
 * at boot) as the other half of addressing the persistent 0x08
 * CONN_TIMEOUTs seen at 20-subevent scale -- see NOTES.md 2026-08-03.
 */
/* Supervision timeout must have real margin over the ~10s post-PAST hold
 * below (per_adv_params.interval_max * 5 / 4), not just equal it -- at
 * equal values, ordinary radio jitter makes the supervision timeout race
 * the intentional disconnect, producing a stray 0x08 CONN_TIMEOUT instead
 * of the clean 0x13/0x16 self-disconnect about half the time (confirmed in
 * logs/peripheral_20260731_111128.log: 0x13/0x08 alternating over 5
 * cycles, both taking ~9.4-11.4s). 18s gives ~8s of margin over the 10s hold.
 */
static struct bt_le_conn_param onboard_conn_param_storage =
	BT_LE_CONN_PARAM_INIT(0x20, 0x20, 0, BT_GAP_MS_TO_CONN_TIMEOUT(18000));
static const struct bt_le_conn_param *onboard_conn_param = &onboard_conn_param_storage;

/* Bound on the main loop's post-bt_conn_le_create() wait (see its k_poll()
 * call below), longer than onboard_conn_param's own 18s supervision timeout
 * so a connection that's slowly-but-actually-establishing isn't punished.
 * A K_FOREVER wait here is a real deadlock, not just a slow path: if the
 * peripheral walks out of range in the window between bt_conn_le_create()
 * succeeding and the link-layer connection actually completing, neither
 * connected_cb nor disconnected_cb ever fires (there's no established
 * connection for either to fire about), scanning is already stopped (see
 * device_found()), and nothing else in this loop can wake it -- the central
 * hangs for every node, not just the one that walked away, previously only
 * recovered via the watchdog's full-board reset (3 min, drops every
 * already-synced node too). 25s gives real margin over the 18s supervision
 * timeout while still recovering in seconds compared to that.
 */
#define connect_wait_timeout_ms 25000

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	char name[NAME_LEN];
	int err;

	if (default_conn) {
		return;
	}

	/* We're only interested in connectable events */
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	(void)memset(name, 0, sizeof(name));
	bt_data_parse(ad, data_cb, name);

	/* Prefix check, not exact match: target_adv_name is built with
	 * node_id=0 (no "#..." suffix, see main()), but a real peripheral's
	 * name always has one (e.g. "PAwR sync sample 2 #47") -- strcmp
	 * would never match anything. strlen(target_adv_name) also correctly
	 * requires at least the separating space before whatever follows, so
	 * this can't accidentally match e.g. central 2 against a "20"-suffix
	 * peripheral name.
	 */
	if (strncmp(name, target_adv_name, strlen(target_adv_name)) != 0) {
		return;
	}

	unsigned int node_id = pawr_parse_node_id(name);
	int slot;

#if CONFIG_APP_ACCEPT_ANY_NODE
	if (next_test_slot >= NUM_SUBEVENTS) {
		printk("Peripheral advertised as \"%s\" (node_id %u) -- out of test subevents (%d used), refusing to onboard. Reflash central to reset.\n",
		       name, node_id, NUM_SUBEVENTS);
		return;
	}
	slot = (int)next_test_slot++;
	printk("[TEST MODE] Assigning node_id %u -> subevent %d (node_slot_table.h ignored)\n",
	       node_id, slot);
#else
	slot = lookup_fixed_slot(node_id);

	if (slot < 0) {
		printk("Peripheral advertised as \"%s\" (node_id %u) has no node_slot_table entry for central %u -- refusing to onboard. Add it to central/node_slot_table.h and reflash.\n",
		       name, node_id, CONFIG_APP_CENTRAL_ID);
		return;
	}
#endif

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Found peripheral %s (node_id %u, fixed subevent %d), connecting...\n", addr_str,
	       node_id, slot);

	if (bt_le_scan_stop()) {
		return;
	}

	pending_slot = slot;

	/* A slower interval + longer supervision timeout than the default
	 * (30-50ms / 4s) gives the controller more slack to service this
	 * onboarding connection around the periodic advertising subevent
	 * train, which is far busier now (20 subevents, 10s interval) than
	 * in the original smoke test -- the default's tight timeout was
	 * getting hit (0x08 CONN_TIMEOUT) under that contention.
	 */
#if APP_STOP_PAWR_DURING_ONBOARDING
	err = bt_le_per_adv_stop(pawr_adv);
	if (err) {
		printk("Failed to stop periodic advertising before connect (err %d)\n", err);
	}
#endif

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, onboard_conn_param,
				&default_conn);
	if (err) {
		printk("Create conn to %s failed (%u)\n", addr_str, err);

		/* connect_pending deliberately left false here: bt_conn_le_create()
		 * doesn't touch its out-param on failure, so default_conn is still
		 * whatever it was before this call (NULL, since device_found()'s
		 * own guard at the top of this function already requires that) --
		 * there is no pending connection for main()'s loop to wait on.
		 * Restarting scanning here lets this node (or any other) be found
		 * and retried on the next advertisement, same as every other
		 * failure path in this onboarding flow already does. err
		 * intentionally not checked here: if THIS also fails, the main
		 * loop's own bt_le_scan_start() retry on its next iteration is the
		 * backstop -- not worth a second failure log for what's already a
		 * rare, already-being-reported error case.
		 */
		(void)bt_le_scan_start(BT_LE_SCAN_PASSIVE_CONTINUOUS, device_found);

		return;
	}

	/* Tells main()'s loop it's now safe (and necessary) to wait on this
	 * attempt -- see connect_pending's own comment for why the loop must
	 * not enter that wait when this is false.
	 */
	connect_pending = true;
}

static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	struct bt_gatt_chrc *chrc;

	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	chrc = (struct bt_gatt_chrc *)attr->user_data;

	if (!bt_uuid_cmp(chrc->uuid, &pawr_char_uuid.uuid)) {
		pawr_attr_handle = chrc->value_handle;

		printk("Characteristic handle: %d\n", pawr_attr_handle);

		k_sem_give(&sem_discovered);
	}

	return BT_GATT_ITER_STOP;
}

static void write_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	if (err) {
		printk("Write failed (err %d)\n", err);

		return;
	}

	k_sem_give(&sem_written);
}

void init_bufs(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(backing_store); i++) {
		backing_store[i][0] = ARRAY_SIZE(backing_store[i]) - 1;
		backing_store[i][1] = BT_DATA_MANUFACTURER_DATA;
		backing_store[i][2] = 0x59; /* Nordic */
		backing_store[i][3] = 0x00;

		net_buf_simple_init_with_data(&bufs[i], &backing_store[i],
					      ARRAY_SIZE(backing_store[i]));
	}
}

/* subevents[0] is always the primary (the node_slot_table.h entry);
 * subevents[1..NUM_REDUNDANT_COPIES-1] are the redundant copies, each at
 * primary + k * NUM_PRIMARY_SLOTS (see common/pawr_protocol.h). Fixed-size
 * array (not just NUM_REDUNDANT_COPIES fields) so this struct's wire
 * layout only needs to change in one place if the copy count changes
 * again -- peripheral/src/main.c's identical copy must match exactly,
 * same as before this struct grew from 2 explicit fields to this.
 */
struct pawr_timing {
	uint8_t subevents[NUM_REDUNDANT_COPIES];
	uint8_t response_slot;
} __packed;

int main(void)
{
	int err;
#if !APP_STOP_PAWR_DURING_ONBOARDING
	struct bt_le_ext_adv *pawr_adv;
#endif
	struct bt_gatt_discover_params discover_params;
	struct bt_gatt_write_params write_params;
	struct pawr_timing sync_config;

	init_bufs();

	for (size_t i = 0; i < ARRAY_SIZE(last_forwarded_seq); i++) {
		last_forwarded_seq[i] = -1;
	}

	printk("Starting Periodic Advertising Demo (central)\n");
	printk("Central ID: %u\n", CONFIG_APP_CENTRAL_ID);

	node_slot_table_validate();

	pawr_format_adv_name(target_adv_name, sizeof(target_adv_name), CONFIG_APP_CENTRAL_ID, 0);
	printk("Onboarding peripherals advertising as \"%s ...\"\n", target_adv_name);

	if (!gpio_is_ready_dt(&tx_led)) {
		printk("TX LED device not ready\n");
		return 0;
	}

	err = gpio_pin_configure_dt(&tx_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("Failed to configure TX LED (err %d)\n", err);
		return 0;
	}

	/* Non-fatal if this fails (e.g. no gateway board wired up yet) --
	 * gateway_uart_tx_send() just no-ops in that case. See gateway_uart_tx.h.
	 */
	gateway_uart_tx_init();

	/* Direct-to-PC fallback (2026-08-30, see gui_uart_tx.h) -- shares this
	 * board's own USB console with gateway_uart_tx_init() above's uart1
	 * link; both are independent, either one working is enough to get data
	 * somewhere. Also non-fatal if it fails.
	 */
	gui_uart_tx_init();

	/* Hardware watchdog (2026-08-30, see watchdog.h) -- last-resort
	 * recovery if the onboarding loop ever gets wedged again. Also
	 * non-fatal if it fails to arm.
	 */
	watchdog_init();

	/* Fallback local record of every payload received over PAwR, in case
	 * the UART link to the gateway board (or the gateway's own MQTT/LTE
	 * hop) is down -- same rationale/mechanism as peripheral's on-board
	 * flash log, see common/sensor_log.h.
	 */
	sensor_log_init();

	if (IS_ENABLED(CONFIG_APP_DUMP_LOG_ON_BOOT)) {
		sensor_log_dump_all();
	}

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	/* Without this, the controller may not cleanly support running the
	 * initiator (onboarding connections) and a PAwR advertiser at the
	 * same time -- this is the SDC's own purpose-built feature for that
	 * exact combination, disabled by default. See NOTES.md 2026-08-03.
	 * Disabled again after any HCI Reset, so must be (re-)enabled here
	 * every boot, before any connection attempts.
	 */
	{
		sdc_hci_cmd_vs_allow_parallel_connection_establishments_t parallel_conn_params = {
			.enable = 1,
		};

		err = hci_vs_sdc_allow_parallel_connection_establishments(&parallel_conn_params);
		if (err) {
			printk("Failed to enable parallel connection establishment (err %d)\n",
			       err);
		}
	}

	/* Create a non-connectable advertising set. */
	err = bt_le_ext_adv_create(BT_LE_EXT_ADV_NCONN, &adv_cb, &pawr_adv);
	if (err) {
		printk("Failed to create advertising set (err %d)\n", err);
		return 0;
	}

	/* Set periodic advertising parameters */
	err = bt_le_per_adv_set_param(pawr_adv, &per_adv_params);
	if (err) {
		printk("Failed to set periodic advertising parameters (err %d)\n", err);
		return 0;
	}

	/* Enable Periodic Advertising */
	printk("Start Periodic Advertising\n");
	err = bt_le_per_adv_start(pawr_adv);
	if (err) {
		printk("Failed to enable periodic advertising (err %d)\n", err);
		return 0;
	}

	/* 2026-08-09: tried staggering radio startup here (50ms, then 500ms
	 * delays between per_adv_start/ext_adv_start/scan_start) while chasing
	 * a PAST sync failure at NUM_SUBEVENTS=34 -- made no measurable
	 * difference either way, and the eventual finding (see NOTES.md
	 * 2026-08-09) was that the SAME known-good 0dBm/34-subevent config
	 * later failed too, meaning none of the config knobs tried that day
	 * (TX power, PAST timeout, event-length budget, this stagger) were
	 * ever the actual variable. Reverted to no artificial delay here to
	 * stop carrying an unproven change forward.
	 */

	printk("Start Extended Advertising\n");
	err = bt_le_ext_adv_start(pawr_adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		printk("Failed to start extended advertising (err %d)\n", err);
		return 0;
	}

	while (true) {
		/* Enable continuous scanning. Retried rather than fatal on
		 * failure (2026-08-31, real hardware): observed err -EALREADY
		 * here immediately after the new connect-timeout path below
		 * canceled a stalled connection attempt via
		 * bt_conn_disconnect() -- the controller can still be mid
		 * teardown of that cancel (radio resource still "in use" from
		 * its point of view) on the very next loop iteration, so the
		 * very first bt_le_scan_start() retry can transiently fail
		 * even though scanning really was stopped (device_found()
		 * calls bt_le_scan_stop() before ever creating a connection).
		 * The old `return 0` here for ANY failure was worse than the
		 * K_FOREVER deadlock this file's other fixes address: it
		 * silently ends main() and stops feeding the watchdog too, so
		 * not even the watchdog's full-reset backstop would recover
		 * it -- the board just goes idle forever. A short retry loop
		 * gives the controller time to settle instead.
		 */
		for (int attempt = 0; attempt < 5; attempt++) {
			err = bt_le_scan_start(BT_LE_SCAN_PASSIVE_CONTINUOUS, device_found);
			if (!err) {
				break;
			}

			printk("Scanning failed to start (err %d), retrying (%d/5)...\n", err,
			       attempt + 1);
			k_sleep(K_MSEC(200));
		}

		if (err) {
			/* Still failing after retries -- likely a real
			 * controller problem, not transient settle time.
			 * Deliberately do NOT feed the watchdog here (contrast
			 * with the retry loop above, which is healthy forward
			 * progress): this state needs the watchdog's full
			 * board reset to recover, since nothing left in this
			 * function can. Sleeping instead of returning keeps
			 * this failure mode visibly "stuck" on the console
			 * during that wait rather than silently exiting main().
			 */
			printk("Scanning still failing to start after retries (err %d) -- waiting for watchdog reset\n",
			       err);
			k_sleep(K_FOREVER);
		}

		printk("Scanning successfully started\n");

		/* Proves the main loop just made real forward progress (see
		 * watchdog.h/.c) -- every iteration reaches here whether this
		 * attempt goes on to succeed or fail, so this is the one feed
		 * point that would stop firing if this loop ever wedges
		 * again the way device_found()'s bt_conn_le_create() failure
		 * path used to (see that function's own comment).
		 */
		watchdog_feed();

		/* Poll for connect_pending becoming true instead of looping back
		 * to the top of the outer while(true) when nothing was found --
		 * the ordinary case once every known node is synced, or whenever
		 * every peripheral is briefly out of range. `continue`-ing back
		 * up there (first cut of this fix, 2026-08-31) re-runs
		 * bt_le_scan_start() on top of scanning that's already active
		 * (device_found() only stops it once it actually finds a
		 * peripheral to connect to), which itself returns -EALREADY --
		 * confirmed on real hardware, immediately after boot, well
		 * before any connection had even been attempted, so this wasn't
		 * the transient "controller still tearing down a cancel" case
		 * the retry loop above's own comment describes. Looping in place
		 * here instead never touches the scanner. Polls on a short sleep
		 * rather than blocking on a semaphore, since connect_pending is a
		 * plain bool flipped from device_found() on the BT RX thread, not
		 * something this loop can k_poll()/k_sem_take() on directly; the
		 * per-iteration watchdog_feed() re-proves forward progress so an
		 * extended idle period here can't starve the watchdog the way the
		 * old unconditional k_poll() bug could.
		 */
		while (!connect_pending) {
			k_sleep(K_MSEC(200));
			watchdog_feed();
		}

		/* Wait for either remote info available or involuntary disconnect,
		 * bounded rather than K_FOREVER -- see connect_wait_timeout_ms's
		 * own comment for why an unbounded wait here is a real deadlock,
		 * not just a slow path.
		 */
		err = k_poll(events, ARRAY_SIZE(events), K_MSEC(connect_wait_timeout_ms));
		if (err == -EAGAIN) {
			/* Neither sem fired in time -- most likely a peripheral
			 * that walked out of range mid-connection-attempt:
			 * bt_conn_le_create() above already succeeded (this
			 * central accepted the attempt), but the link-layer
			 * connection never actually completed, so neither
			 * connected_cb nor disconnected_cb has anything to fire
			 * for. Scanning is already stopped (device_found()),
			 * so without this, the central is wedged for every
			 * node, not just this one -- previously only recovered
			 * via the watchdog's full-board reset (3 min, drops
			 * every already-synced node too). Cancel the stalled
			 * attempt explicitly and fall through the same cleanup
			 * every other early-exit path already uses.
			 */
			printk("Timed out waiting for connection to complete (%d ms) -- canceling stalled attempt\n",
			       connect_wait_timeout_ms);

			err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			if (err != 0 && err != -ENOTCONN) {
				printk("Failed to cancel stalled connection attempt (err %d)\n", err);
			}

			/* bt_conn_disconnect() on a still-connecting conn (Zephyr
			 * host state BT_CONN_INITIATING) sends an HCI LE Create
			 * Connection Cancel command and returns immediately --
			 * that only requests the cancel, it doesn't wait for the
			 * controller to confirm it. The actual confirmation
			 * arrives later as an LE Connection Complete event
			 * (cancelled status), which the host's own
			 * le_conn_complete_cancel() turns into the normal
			 * disconnected_cb callback -- so sem_disconnected DOES
			 * fire here, just not synchronously with this call
			 * returning. Originally assumed (2026-08-31, first cut of
			 * this fix) that it wouldn't fire and skipped straight to
			 * clearing default_conn + retrying bt_le_scan_start() --
			 * that raced ahead of the controller actually finishing
			 * the cancel and hit err -EALREADY on real hardware,
			 * every time, not just transiently (confirmed: a 5x200ms
			 * blind retry loop still failed every attempt). Waiting
			 * for the real completion signal instead of guessing at a
			 * settle delay is the actual fix. Bounded rather than
			 * K_FOREVER as a backstop in case some other cancel path
			 * genuinely never fires it -- if this also times out, the
			 * scan-start retry loop below plus the watchdog remain as
			 * further backstops.
			 */
			err = k_sem_take(&sem_disconnected, K_SECONDS(5));
			if (err) {
				printk("Cancel confirmation timed out -- proceeding anyway\n");
			}

			bt_conn_unref(default_conn);
			default_conn = NULL;
			connect_pending = false;

			continue;
		}

		err = k_sem_take(&sem_connected, K_NO_WAIT);
		if (err) {
			printk("Disconnected before remote info available\n");

			goto disconnected;
		}

		err = bt_le_per_adv_set_info_transfer(pawr_adv, default_conn, 0);
		if (err) {
			printk("Failed to send PAST (err %d)\n", err);

			goto disconnect;
		}

		printk("PAST sent\n");

#if APP_STOP_PAWR_DURING_ONBOARDING
		/* Resume now, not at full onboarding completion -- the
		 * post-PAST hold below needs a live periodic train for the
		 * peripheral to actually sync to.
		 */
		err = bt_le_per_adv_start(pawr_adv);
		if (err) {
			printk("Failed to resume periodic advertising after PAST (err %d)\n", err);
		}
#endif

#if APP_MINIMAL_REPRO
		/* Minimal-repro mode: skip the dynamic GATT slot-assignment
		 * dance entirely and rely on the peripheral's built-in
		 * zero-initialized default (subevent 0 / response slot 0).
		 * See common/pawr_protocol.h and NOTES.md 2026-07-31.
		 */
		goto disconnect;
#endif

		discover_params.uuid = &pawr_char_uuid.uuid;
		discover_params.func = discover_func;
		discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
		discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			printk("Discovery failed (err %d)\n", err);

			goto disconnect;
		}

		printk("Discovery started\n");

		err = k_sem_take(&sem_discovered, K_SECONDS(10));
		if (err) {
			printk("Timed out during GATT discovery\n");

			goto disconnect;
		}

		/* pending_slot was already resolved from node_slot_table.h back
		 * in device_found(), before this connection was even created --
		 * fixed assignment doesn't need anything learned during
		 * discovery/connection to pick a slot, unlike the old dynamic
		 * allocate_slot(bt_conn_get_dst(...)) call this replaced.
		 *
		 * subevents[k] = pending_slot + k * NUM_PRIMARY_SLOTS (see
		 * common/pawr_protocol.h for why this is a fixed offset, not
		 * explicit node_slot_table.h columns): the peripheral answers
		 * whichever of its NUM_REDUNDANT_COPIES assigned subevents'
		 * polls it actually receives each interval, so a response
		 * lost on one has independent further chances on the others
		 * before the next sensor reading replaces the payload.
		 */
#if CONFIG_APP_ACCEPT_ANY_NODE
		/* Test mode: no redundant copies (see Kconfig) -- point every
		 * copy at the same subevent as the primary so the
		 * peripheral's existing multi-slot write path needs no
		 * changes; it just answers that one subevent
		 * NUM_REDUNDANT_COPIES times, which is harmless.
		 */
		for (size_t k = 0; k < NUM_REDUNDANT_COPIES; k++) {
			sync_config.subevents[k] = (uint8_t)pending_slot;
		}
#else
		for (size_t k = 0; k < NUM_REDUNDANT_COPIES; k++) {
			sync_config.subevents[k] = (uint8_t)(pending_slot + k * NUM_PRIMARY_SLOTS);
		}
#endif
		sync_config.response_slot = 0;

		write_params.func = write_func;
		write_params.handle = pawr_attr_handle;
		write_params.offset = 0;
		write_params.data = &sync_config;
		write_params.length = sizeof(sync_config);

		err = bt_gatt_write(default_conn, &write_params);
		if (err) {
			printk("Write failed (err %d)\n", err);

			goto disconnect;
		}

		printk("Write started\n");

		err = k_sem_take(&sem_written, K_SECONDS(10));
		if (err) {
			printk("Timed out during GATT write\n");

			goto disconnect;
		}

		printk("PAwR config written: subevents %d, %d, %d\n", sync_config.subevents[0],
		       sync_config.subevents[1], sync_config.subevents[2]);

disconnect:
		/* Wait slightly longer than one periodic advertising interval
		 * (interval_max is in 1.25ms units) to ensure the peripheral
		 * has actually received at least one periodic advertising
		 * event and established sync before we tear down the
		 * connection PAST was sent over.
		 */
		k_sleep(K_MSEC(per_adv_params.interval_max * 5 / 4));

		err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err != 0 && err != -ENOTCONN) {
			return 0;
		}

disconnected:
#if APP_STOP_PAWR_DURING_ONBOARDING
		/* Catch-all: every path through the loop reaches here. If PAST
		 * was sent successfully, periodic advertising is already
		 * running again (resumed right after "PAST sent" above) and
		 * this is a harmless already-started no-op/error. If we got
		 * here via an early failure (disconnected before remote info,
		 * or PAST send itself failed) it was never resumed -- this is
		 * the only place that covers both without duplicating the
		 * resume call at every early-exit site.
		 */
		(void)bt_le_per_adv_start(pawr_adv);
#endif
		k_sem_take(&sem_disconnected, K_FOREVER);

		/* default_conn can already be NULL reaching this label: the
		 * "Disconnected before remote info available" path above jumps
		 * here directly after connected_cb's own error branch already
		 * did this exact unref+NULL (see that function's comment on why
		 * it also gives sem_disconnected) -- unref-ing again would
		 * double-decrement a real connection's refcount, or (since
		 * bt_conn_unref() asserts/derefs its argument) fault on a NULL
		 * one outright. Every other path reaching this label still has
		 * a live default_conn from bt_conn_disconnect() just above, so
		 * this guard only skips the redundant work, never the needed one.
		 */
		if (default_conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}
		connect_pending = false;
	}

	return 0;
}
