/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE GATT central: skin sensor hub (2026-08-13, replaces the earlier
 * PAwR-based design -- see common/pawr_protocol.h's file header for why).
 *
 * 2026-08-14: operator control moved from a bare 'i'/'d' mode toggle to a
 * line-based command protocol (see console_command_line() below), so an
 * external GUI can target one specific node_id instead of "whichever
 * eligible node shows up next". Commands (newline-terminated, case-sensitive):
 *
 *   SCAN            Report-only: scan and print "EVT SCAN NODE <id> RSSI
 *                   <rssi> STATE <state>" for every matching advertisement
 *                   seen, deduped per node within a short window. Never
 *                   connects. Runs until superseded by another command.
 *   INIT <node_id>  Scan for that node_id only, connect, run the init flow
 *                   (write "start measuring"), disconnect, print a
 *                   structured EVT result line, return to idle.
 *   DOWNLOAD <node_id>   Same, but the download flow.
 *   STOP            Stop scanning, return to idle.
 *
 * Every state transition a controlling GUI needs also prints a single-line,
 * space-delimited "EVT ..." record (see print_evt* helpers) alongside the
 * existing human-oriented prose printk()s, which are unchanged and still the
 * primary output for bench debugging over a raw terminal.
 *
 * CONFIG_BT_MAX_CONN=1 -- one connection, one node, at a time, in both
 * modes (see NOTES.md/the design plan for why this is an acceptable
 * simplification for what is fundamentally a post-hoc, not live/real-time,
 * operation).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "pawr_protocol.h"
#include "console_trigger.h"
#include "gateway_uart_tx.h"
#include "sensor_log.h"

#define NAME_LEN 30

/* This central only onboards/downloads peripherals advertising this exact
 * prefix -- see pawr_format_adv_name() in common/pawr_protocol.h and
 * CONFIG_APP_CENTRAL_ID in Kconfig for why/format. Built once at startup,
 * not per scan callback.
 */
static char target_adv_name[PAWR_ADV_NAME_MAX_LEN];

static const struct gpio_dt_spec tx_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_mtu_exchanged, 0, 1);
static K_SEM_DEFINE(sem_discovered, 0, 1);
static K_SEM_DEFINE(sem_written, 0, 1);
static K_SEM_DEFINE(sem_disconnected, 0, 1);
static K_SEM_DEFINE(sem_download_header, 0, 1);
static K_SEM_DEFINE(sem_download_done, 0, 1);
static K_SEM_DEFINE(sem_download_ccc_written, 0, 1);

static struct bt_uuid_128 pawr_start_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));
static struct bt_uuid_128 pawr_download_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2));
static uint16_t start_attr_handle;
static uint16_t download_attr_handle;

/* ======================================================
 * Operator command protocol: line-based commands over the console (see
 * console_trigger.h for the underlying per-byte UART ISR watcher this
 * assembles into lines). Starts idle -- an external controller (GUI or a
 * human on a raw terminal) issues SCAN/INIT/DOWNLOAD/STOP explicitly, no
 * mode is assumed at boot.
 * ====================================================== */

enum central_op {
	OP_IDLE,
	OP_SCAN_REPORT,
	OP_INIT,
	OP_DOWNLOAD,
};

static enum central_op current_op = OP_IDLE;
static unsigned int target_node_id;

#define CONSOLE_LINE_MAX 32
static char console_line_buf[CONSOLE_LINE_MAX];
static size_t console_line_len;

static const char *node_state_str(unsigned int node_id);

static void print_help(void)
{
	printk("[CMD] Commands: SCAN | INIT <node_id> | DOWNLOAD <node_id> | STOP\n");
}

/* Parses one already-assembled, NUL-terminated command line. Kept as a
 * hand-rolled line parser rather than Zephyr's shell subsystem -- see
 * console_trigger.h's file header for why CONFIG_SHELL was rejected earlier
 * in this project.
 */
static void console_process_line(char *line)
{
	char *cmd = strtok(line, " \t");

	if (!cmd) {
		return;
	}

	if (!strcmp(cmd, "SCAN")) {
		current_op = OP_SCAN_REPORT;
		printk("\n[CMD] SCAN\n");
		return;
	}

	if (!strcmp(cmd, "STOP")) {
		current_op = OP_IDLE;
		printk("\n[CMD] STOP\n");
		return;
	}

	if (!strcmp(cmd, "INIT") || !strcmp(cmd, "DOWNLOAD")) {
		char *arg = strtok(NULL, " \t");
		unsigned long node_id;

		if (!arg) {
			printk("\n[CMD] %s requires a node_id\n", cmd);
			return;
		}

		node_id = strtoul(arg, NULL, 10);
		if (node_id == 0 || node_id > UINT8_MAX) {
			printk("\n[CMD] Invalid node_id \"%s\"\n", arg);
			return;
		}

		target_node_id = (unsigned int)node_id;
		current_op = !strcmp(cmd, "INIT") ? OP_INIT : OP_DOWNLOAD;
		printk("\n[CMD] %s NODE %u\n", cmd, target_node_id);
		return;
	}

	printk("\n[CMD] Unknown command \"%s\"\n", cmd);
	print_help();
}

/* Byte-at-a-time line assembly on top of console_trigger.c's dumb per-byte
 * ISR callback -- keeps console_trigger.c a reusable, command-agnostic byte
 * watcher (matches gateway_9151/src/uart/uart_receiver.c's pattern) while
 * command semantics live entirely here. Runs in UART ISR context like the
 * byte callback it's called from -- console_process_line() only does
 * bounded string parsing and printk(), no blocking calls.
 */
static void console_byte_cb(uint8_t byte)
{
	if (byte == '\r') {
		return;
	}

	if (byte == '\n') {
		console_line_buf[console_line_len] = '\0';
		if (console_line_len > 0) {
			console_process_line(console_line_buf);
		}
		console_line_len = 0;
		return;
	}

	if (console_line_len < CONSOLE_LINE_MAX - 1) {
		console_line_buf[console_line_len++] = (char)byte;
	} else {
		/* Line too long -- drop it rather than overflow, reset and
		 * wait for the next newline to resync.
		 */
		console_line_len = 0;
	}
}

/* ======================================================
 * Per-node state: has this node_id been inited / downloaded from yet, this
 * session? Indexed directly by sensor_payload.node_id's full uint8_t
 * range, same pattern as the old last_forwarded_seq[] this replaces.
 * ====================================================== */

enum node_state {
	NODE_UNKNOWN,
	NODE_INITED,
	NODE_DOWNLOADED,
};

static enum node_state node_states[UINT8_MAX + 1];

static const char *node_state_str(unsigned int node_id)
{
	switch (node_states[node_id]) {
	case NODE_INITED:
		return "INITED";
	case NODE_DOWNLOADED:
		return "DOWNLOADED";
	default:
		return "UNKNOWN";
	}
}

/* SCAN report-only mode dedup: last time (in uptime ms) each node_id was
 * reported, so a continuously-advertising node doesn't flood the console
 * with an EVT line on every single advertisement.
 */
#define SCAN_REPORT_DEDUP_WINDOW_MS 2000
static int64_t scan_last_reported_ms[UINT8_MAX + 1];

static struct bt_conn *default_conn;
static unsigned int pending_node_id;

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

/* Standard BLE connection parameters -- no PAwR-specific tuning needed
 * anymore (the old 40ms-interval/18s-supervision-timeout values existed
 * specifically to survive collisions with a concurrent PAwR subevent
 * train, see git history on this file; there is no such train anymore).
 * BT_LE_CONN_PARAM_DEFAULT is Zephyr's own general-purpose default.
 */
static const struct bt_le_conn_param *conn_param = BT_LE_CONN_PARAM_DEFAULT;

static void scan_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			      struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	char name[NAME_LEN];
	int err;

	if (default_conn) {
		return;
	}

	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	(void)memset(name, 0, sizeof(name));
	bt_data_parse(ad, data_cb, name);

	/* Prefix check, not exact match: target_adv_name is built with
	 * node_id=0 (no "#..." suffix), but a real peripheral's name always
	 * has one (e.g. "PAwR sync sample 2 #47").
	 */
	if (strncmp(name, target_adv_name, strlen(target_adv_name)) != 0) {
		return;
	}

	unsigned int node_id = pawr_parse_node_id(name);

	if (node_id == 0 || node_id > UINT8_MAX) {
		return;
	}

	if (current_op == OP_SCAN_REPORT) {
		int64_t now = k_uptime_get();

		if (now - scan_last_reported_ms[node_id] >= SCAN_REPORT_DEDUP_WINDOW_MS) {
			scan_last_reported_ms[node_id] = now;
			printk("EVT SCAN NODE %u RSSI %d STATE %s\n", node_id, rssi,
			       node_state_str(node_id));
		}
		return;
	}

	bool want_connect = (current_op == OP_INIT || current_op == OP_DOWNLOAD) &&
			    node_id == target_node_id;

	if (!want_connect) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Found peripheral %s (node_id %u), connecting for %s...\n", addr_str, node_id,
	       current_op == OP_INIT ? "INIT" : "DOWNLOAD");
	printk("EVT CONNECTING NODE %u\n", node_id);

	if (bt_le_scan_stop()) {
		return;
	}

	pending_node_id = node_id;

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, conn_param, &default_conn);
	if (err) {
		printk("Create conn to %s failed (%u)\n", addr_str, err);
		printk("EVT ERROR NODE %u MSG \"connect failed\"\n", node_id);
	}
}

static uint8_t discover_start_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   struct bt_gatt_discover_params *params)
{
	struct bt_gatt_chrc *chrc;

	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	chrc = (struct bt_gatt_chrc *)attr->user_data;

	if (!bt_uuid_cmp(chrc->uuid, &pawr_start_char_uuid.uuid)) {
		start_attr_handle = chrc->value_handle;
		printk("Start-measuring characteristic handle: %d\n", start_attr_handle);
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

void connected_cb(struct bt_conn *conn, uint8_t err)
{
	printk("Connected (err 0x%02X)\n", err);

	__ASSERT(conn == default_conn, "Unexpected connected callback");

	if (err) {
		printk("EVT ERROR NODE %u MSG \"connect err 0x%02X\"\n", pending_node_id, err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		return;
	}

	printk("EVT CONNECTED NODE %u\n", pending_node_id);
	k_sem_give(&sem_connected);
}

/* 2026-08-13, found on real hardware: raising CONFIG_BT_L2CAP_TX_MTU/
 * CONFIG_BT_BUF_ACL_RX_SIZE in prj.conf only raises the negotiation
 * CEILING -- the actual runtime negotiation still needs this explicit
 * bt_gatt_exchange_mtu() call, or the connection silently stays at
 * Zephyr's unnegotiated default (23 bytes / 20 usable). This exact lesson
 * was already learned and documented in prj.conf during the earlier
 * PAwR-era backfill work, and was missed when this file was rewritten for
 * the GATT pivot -- confirmed missing entirely (zero references anywhere
 * in this file) while investigating a download transfer stall.
 */
static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	if (err) {
		printk("MTU exchange failed (att err %d), continuing at default MTU\n", err);
	} else {
		printk("MTU exchange complete: %u\n", bt_gatt_get_mtu(conn));
	}

	k_sem_give(&sem_mtu_exchanged);
}

static struct bt_gatt_exchange_params mtu_exchange_params;

static int do_mtu_exchange(void)
{
	mtu_exchange_params.func = mtu_exchange_cb;

	int err = bt_gatt_exchange_mtu(default_conn, &mtu_exchange_params);

	if (err) {
		printk("Failed to start MTU exchange (err %d), continuing at default MTU\n", err);
		return err;
	}

	err = k_sem_take(&sem_mtu_exchanged, K_SECONDS(10));
	if (err) {
		printk("Timed out during MTU exchange, continuing at default MTU\n");
	}

	return 0; /* non-fatal either way -- smaller MTU just means smaller PDUs */
}

void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected, reason 0x%02X %s\n", reason, bt_hci_err_to_str(reason));
	printk("EVT DISCONNECTED NODE %u\n", pending_node_id);

	k_sem_give(&sem_disconnected);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

/* ======================================================
 * INIT mode: connect -> discover start-measuring characteristic -> write
 * (empty payload) -> disconnect.
 * ====================================================== */

static int do_init_flow(void)
{
	struct bt_gatt_discover_params discover_params;
	struct bt_gatt_write_params write_params;
	struct download_req empty_req = { 0 }; /* unused here, write_params.data below is NULL len 0 */
	int err;

	discover_params.uuid = &pawr_start_char_uuid.uuid;
	discover_params.func = discover_start_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	err = bt_gatt_discover(default_conn, &discover_params);
	if (err) {
		printk("Discovery failed (err %d)\n", err);
		return err;
	}

	err = k_sem_take(&sem_discovered, K_SECONDS(10));
	if (err) {
		printk("Timed out during GATT discovery\n");
		return -ETIMEDOUT;
	}

	write_params.func = write_func;
	write_params.handle = start_attr_handle;
	write_params.offset = 0;
	write_params.data = &empty_req; /* content ignored by peripheral, len must be 0 */
	write_params.length = 0;

	err = bt_gatt_write(default_conn, &write_params);
	if (err) {
		printk("Write failed (err %d)\n", err);
		return err;
	}

	err = k_sem_take(&sem_written, K_SECONDS(10));
	if (err) {
		printk("Timed out during GATT write\n");
		return -ETIMEDOUT;
	}

	printk("Node %u: start-measuring write confirmed\n", pending_node_id);
	return 0;
}

/* ======================================================
 * DOWNLOAD mode: connect -> discover download characteristic + its CCC ->
 * enable indications -> write request -> drain header/data/done
 * indications -> disconnect. Adapted from the backfill-ble-retrieval
 * branch's already-proven design (see common/pawr_protocol.h).
 * ====================================================== */

static uint16_t download_total_entries;
static uint16_t download_received_count;
static uint16_t download_entries_sent; /* from the DONE message itself --
					 * see its use below for why this, not
					 * download_total_entries, is the right
					 * thing to compare against
					 * download_received_count.
					 */
static bool download_failed;

static uint8_t discover_download_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				      struct bt_gatt_discover_params *params)
{
	struct bt_gatt_chrc *chrc;

	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	chrc = (struct bt_gatt_chrc *)attr->user_data;

	if (!bt_uuid_cmp(chrc->uuid, &pawr_download_char_uuid.uuid)) {
		download_attr_handle = chrc->value_handle;
		printk("Download characteristic handle: %d\n", download_attr_handle);
		k_sem_give(&sem_discovered);
	}

	return BT_GATT_ITER_STOP;
}

static void download_subscribe_cb(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_subscribe_params *params)
{
	if (err) {
		printk("Failed to enable download indications (att err %d)\n", err);
		download_failed = true;
	}

	k_sem_give(&sem_download_ccc_written);
}

static uint8_t download_notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
				    const void *data, uint16_t length)
{
	if (!data) {
		/* Subscription was removed (e.g. disconnect) */
		return BT_GATT_ITER_STOP;
	}

	if (length < 1) {
		return BT_GATT_ITER_CONTINUE;
	}

	uint8_t msg_type = ((const uint8_t *)data)[0];

	switch (msg_type) {
	case DOWNLOAD_MSG_HEADER: {
		if (length < sizeof(struct download_header)) {
			break;
		}
		struct download_header header;

		memcpy(&header, data, sizeof(header));
		download_total_entries = header.total_entries;
		printk("Download header: %u entries\n", download_total_entries);
		k_sem_give(&sem_download_header);
		break;
	}
	case DOWNLOAD_MSG_DATA: {
		if (length < offsetof(struct download_data_pdu, entries)) {
			break;
		}
		const struct download_data_pdu *pdu = data;
		size_t max_entries =
			(length - offsetof(struct download_data_pdu, entries)) /
			sizeof(struct sensor_payload);
		size_t count = MIN(pdu->count, max_entries);

		for (size_t i = 0; i < count; i++) {
			struct sensor_payload payload;

			memcpy(&payload, &pdu->entries[i], sizeof(payload));

			/* Same downstream path a live reading used to take --
			 * forward to the gateway over UART and append to
			 * central's own fallback flash log.
			 */
			gateway_uart_tx_send(&payload);
			sensor_log_append(&payload);

			printk("  [%u] node %u seq %u t+%ums temp=%d.%02uC humidity=%u.%u%%\n",
			       download_received_count, payload.node_id, payload.seq,
			       payload.millis_since_init, payload.temp_cdeg / 100,
			       abs(payload.temp_cdeg % 100), payload.humidity_pct10 / 10,
			       payload.humidity_pct10 % 10);
			printk("EVT DOWNLOAD_DATA NODE %u SEQ %u TEMP %d HUM %u MS %u\n",
			       payload.node_id, payload.seq, payload.temp_cdeg,
			       payload.humidity_pct10, payload.millis_since_init);

			download_received_count++;
		}
		break;
	}
	case DOWNLOAD_MSG_DONE: {
		if (length >= sizeof(struct download_done)) {
			struct download_done done;

			memcpy(&done, data, sizeof(done));
			download_entries_sent = done.entries_sent;
		} else {
			/* Malformed/truncated DONE -- fall back to the
			 * (possibly stale) header count rather than treat this
			 * as "sent 0", which would fail a transfer that
			 * otherwise genuinely succeeded.
			 */
			download_entries_sent = download_total_entries;
		}

		printk("Download done: %u entries received, peripheral confirms sending %u (header originally estimated %u)\n",
		       download_received_count, download_entries_sent, download_total_entries);
		k_sem_give(&sem_download_done);
		break;
	}
	default:
		break;
	}

	return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params download_subscribe_params;

/* bt_gatt_subscribe()'s auto-discover-CCC path (ccc_handle ==
 * BT_GATT_AUTO_DISCOVER_CCC_HANDLE, see CONFIG_BT_GATT_AUTO_DISCOVER_CCC in
 * prj.conf) needs BOTH params->end_handle set AND params->disc_params
 * pointing at real storage it can populate and run its own internal
 * bt_gatt_discover() through (see gatt_ccc_discover() in Zephyr's
 * subsys/bluetooth/host/gatt.c -- it memsets *disc_params itself). Found
 * the hard way on real hardware: omitting these two fields doesn't crash,
 * it fails fast with -EINVAL from bt_gatt_subscribe() itself, every single
 * attempt. Static, not stack-local, for the same reason
 * download_subscribe_params is -- this discovery is asynchronous and must
 * stay valid past this function returning.
 */
static struct bt_gatt_discover_params download_ccc_disc_params;

static int do_download_flow(void)
{
	struct bt_gatt_discover_params discover_params;
	struct bt_gatt_write_params write_params;
	struct download_req req = { 0 };
	int err;

	download_total_entries = 0;
	download_received_count = 0;
	download_entries_sent = 0;
	download_failed = false;

	discover_params.uuid = &pawr_download_char_uuid.uuid;
	discover_params.func = discover_download_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	err = bt_gatt_discover(default_conn, &discover_params);
	if (err) {
		printk("Discovery failed (err %d)\n", err);
		return err;
	}

	err = k_sem_take(&sem_discovered, K_SECONDS(10));
	if (err) {
		printk("Timed out during GATT discovery\n");
		return -ETIMEDOUT;
	}

	download_subscribe_params.notify = download_notify_func;
	download_subscribe_params.subscribe = download_subscribe_cb;
	download_subscribe_params.value_handle = download_attr_handle;
	/* ccc_handle left as BT_GATT_AUTO_DISCOVER_CCC_HANDLE (see
	 * CONFIG_BT_GATT_AUTO_DISCOVER_CCC in prj.conf) -- lets
	 * bt_gatt_subscribe() find the CCC descriptor itself, no separate
	 * discovery pass needed on this app's part. end_handle/disc_params
	 * are what IT needs to actually do that search -- see this function's
	 * download_ccc_disc_params comment above.
	 */
	download_subscribe_params.ccc_handle = 0;
	download_subscribe_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	download_subscribe_params.disc_params = &download_ccc_disc_params;
	download_subscribe_params.value = BT_GATT_CCC_INDICATE;

	err = bt_gatt_subscribe(default_conn, &download_subscribe_params);
	if (err) {
		printk("Failed to subscribe to download indications (err %d)\n", err);
		return err;
	}

	err = k_sem_take(&sem_download_ccc_written, K_SECONDS(10));
	if (err || download_failed) {
		printk("Timed out or failed enabling download indications\n");
		return -ETIMEDOUT;
	}

	write_params.func = write_func;
	write_params.handle = download_attr_handle;
	write_params.offset = 0;
	write_params.data = &req;
	write_params.length = sizeof(req);

	err = bt_gatt_write(default_conn, &write_params);
	if (err) {
		printk("Download request write failed (err %d)\n", err);
		return err;
	}

	err = k_sem_take(&sem_written, K_SECONDS(10));
	if (err) {
		printk("Timed out writing download request\n");
		return -ETIMEDOUT;
	}

	/* Header first, then (if there's anything to send) wait for the done
	 * marker -- a generous but bounded timeout scaled loosely to a large
	 * transfer taking a while; this is a one-time post-experiment
	 * operation, not latency-sensitive.
	 */
	err = k_sem_take(&sem_download_header, K_SECONDS(15));
	if (err) {
		printk("Timed out waiting for download header\n");
		return -ETIMEDOUT;
	}

	err = k_sem_take(&sem_download_done, K_MINUTES(5));
	if (err) {
		printk("Timed out waiting for download to finish (%u/%u entries received)\n",
		       download_received_count, download_total_entries);
		return -ETIMEDOUT;
	}

	/* Compare against entries_sent from the DONE message, NOT the earlier
	 * header's total_entries -- found on real hardware (2026-08-13): the
	 * header count and the actual send pass are two separate fcb_walk()
	 * calls on the peripheral, over a flash log that keeps growing the
	 * whole time (sensor_read_work never pauses for a download, by
	 * design -- see pawr_protocol.h's file header). If a reading lands
	 * in the gap between those two passes, the peripheral genuinely,
	 * correctly sends one more entry than its own header predicted --
	 * that's not data loss, it's the header being a stale snapshot.
	 * entries_sent is what the peripheral actually attempted to
	 * transmit, which is the only number download_received_count should
	 * ever be judged against.
	 */
	if (download_received_count != download_entries_sent) {
		printk("WARNING: node %u download incomplete -- received %u of %u entries the peripheral confirmed sending\n",
		       pending_node_id, download_received_count, download_entries_sent);
		return -EIO;
	}

	printk("Node %u: download complete, %u entries\n", pending_node_id,
	       download_received_count);
	return 0;
}

int main(void)
{
	int err;

	printk("Starting BLE GATT sensor hub (central)\n");
	printk("Central ID: %u\n", CONFIG_APP_CENTRAL_ID);

	for (size_t i = 0; i < ARRAY_SIZE(node_states); i++) {
		node_states[i] = NODE_UNKNOWN;
	}

	pawr_format_adv_name(target_adv_name, sizeof(target_adv_name), CONFIG_APP_CENTRAL_ID, 0);
	printk("Looking for peripherals advertising as \"%s ...\"\n", target_adv_name);

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
	 * gateway_uart_tx_send() just no-ops in that case.
	 */
	gateway_uart_tx_init();

	/* Fallback local record of every downloaded reading, in case the
	 * UART link to the gateway board (or the gateway's own MQTT/LTE hop)
	 * is down.
	 */
	sensor_log_init();

	if (IS_ENABLED(CONFIG_APP_DUMP_LOG_ON_BOOT)) {
		sensor_log_dump_all();
	}

	/* Non-fatal if this fails -- without it, the operator just can't issue
	 * commands at runtime (stuck idle forever), everything else keeps
	 * working.
	 */
	console_trigger_init(console_byte_cb);
	print_help();

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	while (true) {
		err = bt_le_scan_start(BT_LE_SCAN_PASSIVE_CONTINUOUS, scan_device_found);
		if (err) {
			printk("Scanning failed to start (err %d)\n", err);
			return 0;
		}

		k_sem_take(&sem_connected, K_FOREVER);

		gpio_pin_set_dt(&tx_led, 1);

		do_mtu_exchange();

		enum central_op op_at_connect_time = current_op;
		int flow_err;

		if (op_at_connect_time == OP_INIT) {
			flow_err = do_init_flow();
		} else {
			flow_err = do_download_flow();
		}

		if (flow_err == 0) {
			node_states[pending_node_id] =
				(op_at_connect_time == OP_INIT) ? NODE_INITED : NODE_DOWNLOADED;
			if (op_at_connect_time == OP_INIT) {
				printk("EVT INIT_OK NODE %u\n", pending_node_id);
			} else {
				printk("EVT DOWNLOAD_OK NODE %u ENTRIES %u\n", pending_node_id,
				       download_received_count);
			}
		} else {
			printk("Node %u: %s failed (err %d)\n", pending_node_id,
			       op_at_connect_time == OP_INIT ? "init" : "download", flow_err);
			printk("EVT ERROR NODE %u MSG \"%s failed err %d\"\n", pending_node_id,
			       op_at_connect_time == OP_INIT ? "init" : "download", flow_err);
		}

		/* One-shot command consumed -- return to idle so the operator
		 * (or GUI) must explicitly issue the next INIT/DOWNLOAD rather
		 * than this auto-retrying against the same or a different
		 * node indefinitely.
		 */
		current_op = OP_IDLE;

		gpio_pin_set_dt(&tx_led, 0);

		err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err != 0 && err != -ENOTCONN) {
			printk("Disconnect failed (err %d)\n", err);
		}

		k_sem_take(&sem_disconnected, K_FOREVER);

		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	return 0;
}
