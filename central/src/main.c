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
 * Assignment is dynamic and hub-driven (not baked into peripheral firmware)
 * so that later work can add slot-shifting (reassigning a node's subevent
 * at runtime) and flexible node-count membership without redesigning the
 * onboarding channel -- the GATT write used here for initial assignment is
 * the same mechanism a future scheduler would reuse.
 */

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "pawr_protocol.h"

#define PACKET_SIZE 5
#define NAME_LEN    30

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
 * Slot bookkeeping: which subevents are currently assigned, and when we
 * last heard a response on each, so a peripheral that drops off can have
 * its slot reclaimed instead of capacity leaking forever.
 * ====================================================== */

static bool slot_taken[NUM_SUBEVENTS];
static int64_t last_seen_ms[NUM_SUBEVENTS];
static bt_addr_le_t slot_owner[NUM_SUBEVENTS];

static bool slot_is_stale(size_t idx)
{
	if (!slot_taken[idx]) {
		return false;
	}

	int64_t stale_after_ms = (int64_t)PAWR_INTERVAL_UNITS * 5 / 4 *
				  PAWR_SLOT_STALE_INTERVALS;

	return (k_uptime_get() - last_seen_ms[idx]) > stale_after_ms;
}

/* Reuse the same subevent for a peripheral that already has one (identified
 * by its Bluetooth address), rather than handing out a fresh slot on every
 * reconnect -- otherwise a peripheral that repeatedly fails to sync (and
 * keeps reconnecting to retry) leaks a new slot each attempt while its
 * still-live previous assignment sits unused until it goes stale.
 */
static int allocate_slot(const bt_addr_le_t *addr)
{
	for (size_t i = 0; i < NUM_SUBEVENTS; i++) {
		if (slot_taken[i] && !slot_is_stale(i) && bt_addr_le_eq(&slot_owner[i], addr)) {
			last_seen_ms[i] = k_uptime_get();
			return (int)i;
		}
	}

	for (size_t i = 0; i < NUM_SUBEVENTS; i++) {
		if (!slot_taken[i] || slot_is_stale(i)) {
			slot_taken[i] = true;
			last_seen_ms[i] = k_uptime_get();
			bt_addr_le_copy(&slot_owner[i], addr);
			return (int)i;
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

	if (info->subevent < NUM_SUBEVENTS) {
		slot_taken[info->subevent] = true;
		last_seen_ms[info->subevent] = k_uptime_get();
	}

	printk(">>> Node %02u (subevent %d): skin_temp=%d.%02uC humidity=%u.%u%% seq=%u",
	       payload.node_id, info->subevent,
	       payload.temp_cdeg / 100, abs(payload.temp_cdeg % 100),
	       payload.humidity_pct10 / 10, payload.humidity_pct10 % 10, payload.seq);

	if (payload.flags & SENSOR_PAYLOAD_FLAG_TEMP_INVALID) {
		printk("  [FLAG: TEMP_FAIL]");
	}
	if (payload.flags & SENSOR_PAYLOAD_FLAG_HUMIDITY_INVALID) {
		printk("  [FLAG: HUMIDITY_FAIL]");
	}
	printk("\n");
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

/* 100-150ms interval / 10s supervision timeout, versus the library
 * default's 30-50ms / 4s -- gives the controller far more slack to service
 * this onboarding connection around the periodic advertising subevent
 * train (20 subevents every 10s), which is much busier than a plain
 * connectable-advertising peripheral would be.
 */
static struct bt_le_conn_param onboard_conn_param_storage =
	BT_LE_CONN_PARAM_INIT(0x50, 0x78, 0, BT_GAP_MS_TO_CONN_TIMEOUT(10000));
static const struct bt_le_conn_param *onboard_conn_param = &onboard_conn_param_storage;

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

	if (strcmp(name, PAWR_ADV_NAME)) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Found peripheral %s, connecting...\n", addr_str);

	if (bt_le_scan_stop()) {
		return;
	}

	/* A slower interval + longer supervision timeout than the default
	 * (30-50ms / 4s) gives the controller more slack to service this
	 * onboarding connection around the periodic advertising subevent
	 * train, which is far busier now (20 subevents, 10s interval) than
	 * in the original smoke test -- the default's tight timeout was
	 * getting hit (0x08 CONN_TIMEOUT) under that contention.
	 */
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, onboard_conn_param,
				&default_conn);
	if (err) {
		printk("Create conn to %s failed (%u)\n", addr_str, err);
	}
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

struct pawr_timing {
	uint8_t subevent;
	uint8_t response_slot;
} __packed;

int main(void)
{
	int err;
	struct bt_le_ext_adv *pawr_adv;
	struct bt_gatt_discover_params discover_params;
	struct bt_gatt_write_params write_params;
	struct pawr_timing sync_config;

	init_bufs();

	printk("Starting Periodic Advertising Demo (central)\n");

	if (!gpio_is_ready_dt(&tx_led)) {
		printk("TX LED device not ready\n");
		return 0;
	}

	err = gpio_pin_configure_dt(&tx_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("Failed to configure TX LED (err %d)\n", err);
		return 0;
	}

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	/* Create a non-connectable advertising set */
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

	printk("Start Extended Advertising\n");
	err = bt_le_ext_adv_start(pawr_adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		printk("Failed to start extended advertising (err %d)\n", err);
		return 0;
	}

	while (true) {
		/* Enable continuous scanning */
		err = bt_le_scan_start(BT_LE_SCAN_PASSIVE_CONTINUOUS, device_found);
		if (err) {
			printk("Scanning failed to start (err %d)\n", err);
			return 0;
		}

		printk("Scanning successfully started\n");

		/* Wait for either remote info available or involuntary disconnect */
		k_poll(events, ARRAY_SIZE(events), K_FOREVER);
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

		int slot = allocate_slot(bt_conn_get_dst(default_conn));

		if (slot < 0) {
			printk("No free subevent slots available (capacity %d)\n", NUM_SUBEVENTS);

			goto disconnect;
		}

		sync_config.subevent = (uint8_t)slot;
		sync_config.response_slot = 0;

		write_params.func = write_func;
		write_params.handle = pawr_attr_handle;
		write_params.offset = 0;
		write_params.data = &sync_config;
		write_params.length = sizeof(sync_config);

		err = bt_gatt_write(default_conn, &write_params);
		if (err) {
			printk("Write failed (err %d)\n", err);
			slot_taken[slot] = false;

			goto disconnect;
		}

		printk("Write started\n");

		err = k_sem_take(&sem_written, K_SECONDS(10));
		if (err) {
			printk("Timed out during GATT write\n");
			slot_taken[slot] = false;

			goto disconnect;
		}

		printk("PAwR config written: subevent %d\n", slot);

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
		k_sem_take(&sem_disconnected, K_FOREVER);

		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	return 0;
}
