/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE GATT peripheral: skin sensor node (2026-08-13, replaces the earlier
 * PAwR-based design -- see common/pawr_protocol.h's file header for why).
 *
 * Flow: connectable-advertise as "PAwR sync sample" (legacy name, kept for
 * continuity) -> central connects once during the INIT phase and writes the
 * "start measuring" characteristic (no payload, the write itself is the
 * signal) -> this node records k_uptime_get() as its own t0 and starts a
 * 10s periodic sensor-read timer, appending every reading to its on-board
 * flash log -> node keeps measuring/storing/advertising indefinitely,
 * completely independent of whether a connection exists -> whenever central
 * reconnects later (DOWNLOAD phase) and writes the "download" characteristic,
 * this node streams its entire flash log back as a sequence of GATT
 * indications.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "pawr_protocol.h"

/* Diagnostic output toggle: CONFIG_APP_SERIAL_LOGGING defaults to y for
 * development, but should be set to n for real deployment (many unattended
 * field nodes) to avoid any chance of the serial console -- a USB-CDC
 * transport already shown fragile under load (see NOTES.md 2026-08-03) --
 * being a source of problems at all. IS_ENABLED() makes the disabled
 * branch dead code eliminated at compile time, not a runtime check, so
 * this has zero cost when off.
 */
#define APP_LOG(fmt, ...)                                                                        \
	do {                                                                                       \
		if (IS_ENABLED(CONFIG_APP_SERIAL_LOGGING)) {                                      \
			printk(fmt, ##__VA_ARGS__);                                               \
		}                                                                                  \
	} while (0)

#define NAME_LEN 30

/* Sensor read cadence -- was PAWR_INTERVAL_MS (tied to the now-removed PAwR
 * periodic advertising interval); kept as the same 10s value since that's
 * an independently sensible sampling rate for this use case, just no
 * longer coupled to any BLE timing at all.
 */
#define SENSOR_READ_INTERVAL_MS 10000

static K_SEM_DEFINE(sem_disconnected, 0, 1);

static struct bt_conn *default_conn;

/* Status LED: off = not yet init'd (not measuring), steady on = measuring.
 * Distinct from the old PAwR-sync meaning, same physical LED/purpose
 * (visible at-a-glance node state).
 */
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void status_led_init(void)
{
	if (!gpio_is_ready_dt(&status_led)) {
		APP_LOG("Status LED device not ready\n");
		return;
	}

	gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
}

/* Power-on indicator: a single blink of the (unused otherwise) green LED
 * right at boot, so a board is visibly alive the moment it's powered --
 * distinct from status_led (red, led0) above. Blocking sleep is fine here:
 * this runs once in main(), before Bluetooth/sensors start.
 */
static const struct gpio_dt_spec power_on_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void power_on_led_blink(void)
{
	if (!gpio_is_ready_dt(&power_on_led)) {
		APP_LOG("Power-on LED device not ready\n");
		return;
	}

	gpio_pin_configure_dt(&power_on_led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&power_on_led, 1);
	k_sleep(K_MSEC(150));
	gpio_pin_set_dt(&power_on_led, 0);
}

/* ======================================================
 * SENSORS: skin temperature (MAX30205, via the LM75-compatible in-tree
 * driver) + humidity (SHT4x). Each tracked independently so one sensor
 * failing doesn't block reporting the other.
 * ====================================================== */

static const struct device *const dev_temp = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(max30205));
static const struct device *const dev_sht4x = DEVICE_DT_GET_ANY(sensirion_sht4x);

/* The in-tree "lm75" driver is register-compatible with the MAX30205 (same
 * temperature register layout/address), which is why it works at all, but
 * its sensor_channel_get() conversion (lm75_temp_to_sensor_value() in
 * lm75.c) is written for genuine LM75 hardware's 9-bit/0.5C resolution --
 * it right-shifts the raw 16-bit register by 7 bits before converting,
 * discarding almost all precision. The MAX30205 itself has real 16-bit/
 * (1/256)C = ~0.0039C resolution per its datasheet, and our wire format
 * (int16_t temp_cdeg, centi-degrees) already has ample room for that -- so
 * this reads the same physical register directly, independent of the lm75
 * driver instance, to recover the sensor's actual resolution instead of
 * changing the wire format at all. See NOTES.md 2026-08-03.
 */
#define MAX30205_REG_TEMP 0x00
static const struct i2c_dt_spec max30205_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(max30205));

static bool temp_ok;
static bool sht4x_ok;

/* Reads the MAX30205's raw 16-bit temperature register and converts
 * straight to centi-degrees C using its real 1/256C LSB, bypassing the
 * lm75 driver's lossy 0.5C-resolution conversion. Returns 0 on success.
 */
static int max30205_read_temp_cdeg(int16_t *out_cdeg)
{
	uint8_t buf[2];
	int err;

	err = i2c_burst_read_dt(&max30205_i2c, MAX30205_REG_TEMP, buf, sizeof(buf));
	if (err) {
		return err;
	}

	int16_t raw = (int16_t)sys_get_be16(buf);

	/* raw * (1/256) C, converted to centi-degrees: raw * 100 / 256. Use
	 * a wider intermediate type to avoid overflow (raw can be up to
	 * ~16000 in magnitude for realistic skin-temperature ranges, and
	 * *100 could otherwise approach int16_t's range).
	 */
	*out_cdeg = (int16_t)(((int32_t)raw * 100) / 256);

	return 0;
}

static uint16_t s_seq;
static struct sensor_payload latest_payload;

/* Set once, at the init-phase GATT write (see write_start_measuring()) --
 * every subsequent reading's millis_since_init is k_uptime_get() - t0_ms.
 * Until init happens, the node is not measuring at all (see main()'s
 * comment on why sensor_read_work is not scheduled until then).
 */
static int64_t t0_ms;
static bool measuring;

static void sensors_init(void)
{
	if (IS_ENABLED(CONFIG_APP_SIMULATE_SENSORS)) {
		APP_LOG("[SENSORS] Simulated mode enabled, skipping real sensor init\n");
		return;
	}

	/* Still checked via the lm75 driver's own device handle even though
	 * actual reads bypass it (see max30205_read_temp_cdeg) -- this
	 * confirms the shared I2C bus/device came up correctly at boot,
	 * which the raw register read also depends on.
	 */
	if (dev_temp && device_is_ready(dev_temp)) {
		temp_ok = true;
	} else {
		APP_LOG("[WARN] Skin temperature sensor not ready\n");
	}

	if (dev_sht4x && device_is_ready(dev_sht4x)) {
		sht4x_ok = true;
	} else {
		APP_LOG("[WARN] Humidity sensor not ready\n");
	}
}

static void sensors_read(struct sensor_payload *out)
{
	uint8_t flags = 0;
	int16_t temp_cdeg = 0;
	uint16_t humidity_pct10 = 0;

	if (IS_ENABLED(CONFIG_APP_SIMULATE_SENSORS)) {
		/* Slowly drifting plausible fake values, purely for exercising
		 * the data path without any I2C hardware attached.
		 */
		temp_cdeg = 3600 + (s_seq % 20);
		humidity_pct10 = 400 + (s_seq % 50);
	} else {
		struct sensor_value val;

		if (!temp_ok || max30205_read_temp_cdeg(&temp_cdeg) != 0) {
			flags |= SENSOR_PAYLOAD_FLAG_TEMP_INVALID;
		}

		if (sht4x_ok && sensor_sample_fetch(dev_sht4x) == 0 &&
		    sensor_channel_get(dev_sht4x, SENSOR_CHAN_HUMIDITY, &val) == 0) {
			humidity_pct10 = (uint16_t)sensor_value_to_deci(&val);
		} else {
			flags |= SENSOR_PAYLOAD_FLAG_HUMIDITY_INVALID;
		}
	}

	out->node_id = CONFIG_APP_NODE_ID;
	out->flags = flags;
	out->seq = s_seq++;
	out->temp_cdeg = temp_cdeg;
	out->humidity_pct10 = humidity_pct10;
	out->millis_since_init = (uint32_t)(k_uptime_get() - t0_ms);
}

/* ======================================================
 * ON-BOARD FLASH LOG (the node's complete, authoritative record -- not a
 * fallback anymore, the ONLY way data leaves a node until the download
 * phase; see file header)
 *
 * The board's internal "Storage" devicetree partition (32KB, separate from
 * application code -- see nrf52840_partition_uf2_sdv7.dtsi) is used as a
 * Flash Circular Buffer: every sensor_payload produced is appended here.
 * At the current 10s interval, a full 4-hour run is ~1440 records * 12
 * bytes = ~16.9KB, well under the 32KB partition -- no wraparound expected
 * in normal use, but if the buffer does fill, FCB's circular behavior means
 * oldest records are overwritten first, not that appends start failing.
 * ====================================================== */

#define STORAGE_FCB_SECTOR_MAX 8 /* 32KB partition / 4KB pages, see devicetree */

static struct flash_sector storage_fcb_sectors[STORAGE_FCB_SECTOR_MAX];
static struct fcb storage_fcb;
static bool storage_fcb_ok;

static void storage_fcb_init(void)
{
	uint32_t sector_cnt = ARRAY_SIZE(storage_fcb_sectors);
	int err;

	err = flash_area_get_sectors(FIXED_PARTITION_ID(storage_partition), &sector_cnt,
				      storage_fcb_sectors);
	if (err) {
		APP_LOG("[STORAGE] Failed to get flash sectors (err %d)\n", err);
		return;
	}

	storage_fcb = (struct fcb){
		.f_magic = 0x50415752, /* "PAWR", arbitrary non-0xFFFFFFFF marker */
		.f_sector_cnt = sector_cnt,
		.f_sectors = storage_fcb_sectors,
	};

	err = fcb_init(FIXED_PARTITION_ID(storage_partition), &storage_fcb);
	if (err == -ENOMSG) {
		/* -ENOMSG means a sector's on-flash header magic matched
		 * neither "erased" nor our own magic -- i.e. this partition
		 * holds leftover data from something else, not a truly blank
		 * area. Standard FCB recovery: erase the whole partition
		 * once and retry fcb_init(), same as formatting a blank area.
		 */
		const struct flash_area *fap;

		APP_LOG("[STORAGE] Flash log area has foreign data, erasing and retrying\n");

		err = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fap);
		if (!err) {
			err = flash_area_erase(fap, 0, fap->fa_size);
			flash_area_close(fap);
		}

		if (err) {
			APP_LOG("[STORAGE] Failed to erase flash log area (err %d)\n", err);
			return;
		}

		err = fcb_init(FIXED_PARTITION_ID(storage_partition), &storage_fcb);
	}

	if (err) {
		APP_LOG("[STORAGE] Failed to init flash log (err %d)\n", err);
		return;
	}

	storage_fcb_ok = true;
	APP_LOG("[STORAGE] Flash log ready (%u sectors)\n", sector_cnt);
}

/* Proactive rotation: check/rotate BEFORE the log is actually full, so the
 * erase always has a full spare sector of headroom rather than ever
 * happening under write pressure. Proven on real hardware 2026-08-11 (see
 * NOTES.md) -- two earlier reactive-rotation attempts both caused problems
 * for reasons never fully root-caused; this strategy held up cleanly.
 */
#define STORAGE_FCB_ROTATE_FREE_SECTOR_THRESHOLD 2

/* Appends one payload to the flash log. This is now the primary/only data
 * path (see file header) -- failure here is still just logged, not fatal
 * to the node's own operation, but there is no other copy of this reading
 * anywhere once it's gone.
 */
static void storage_fcb_append(const struct sensor_payload *payload)
{
	struct fcb_entry loc;
	int err;

	if (!storage_fcb_ok) {
		return;
	}

	if (fcb_free_sector_cnt(&storage_fcb) <= STORAGE_FCB_ROTATE_FREE_SECTOR_THRESHOLD) {
		err = fcb_rotate(&storage_fcb);
		if (err) {
			APP_LOG("[STORAGE] fcb_rotate failed (err %d)\n", err);
		} else {
			APP_LOG("[STORAGE] fcb_rotate: log wrapped, oldest sector reclaimed\n");
		}
	}

	err = fcb_append(&storage_fcb, sizeof(*payload), &loc);
	if (err) {
		APP_LOG("[STORAGE] fcb_append failed (err %d)\n", err);
		return;
	}

	err = flash_area_write(storage_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), payload,
				sizeof(*payload));
	if (err) {
		APP_LOG("[STORAGE] flash_area_write failed (err %d)\n", err);
		return;
	}

	err = fcb_append_finish(&storage_fcb, &loc);
	if (err) {
		APP_LOG("[STORAGE] fcb_append_finish failed (err %d)\n", err);
	}
}

/* Bench-debugging fallback, independent of the BLE download path: prints
 * the whole flash log as CSV over the plain printk() console. Gated by
 * CONFIG_APP_DUMP_ON_BOOT: build with that set, flash the specific board
 * whose log you want, and capture its serial output right after boot
 * (tools/capture_flash_dump.py -- Watch-SerialLog.ps1's line-based reader
 * drops rows under a large burst, see BUILD_AND_FLASH.md).
 */
struct storage_dump_ctx {
	uint32_t count;
};

static int storage_dump_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
	struct storage_dump_ctx *ctx = arg;
	struct sensor_payload payload;
	int err;

	if (loc_ctx->loc.fe_data_len != sizeof(payload)) {
		/* Skip anything that isn't one of our own fixed-size
		 * records (shouldn't normally happen, but fcb_walk() just
		 * walks whatever is on flash).
		 */
		return 0;
	}

	err = flash_area_read(loc_ctx->fap, FCB_ENTRY_FA_DATA_OFF(loc_ctx->loc), &payload,
			       sizeof(payload));
	if (err) {
		printk("# read error at entry %u (err %d)\n", ctx->count, err);
		return 0;
	}

	printk("%u,%u,0x%02x,%u,%d.%02u,%u.%u,%u\n", payload.node_id, payload.seq, payload.flags,
	       ctx->count, payload.temp_cdeg / 100, abs(payload.temp_cdeg % 100),
	       payload.humidity_pct10 / 10, payload.humidity_pct10 % 10,
	       payload.millis_since_init);

	/* Throttle: printing a large log (thousands of rows) back-to-back
	 * outpaces the console's internal buffer, which silently drops
	 * messages. Sleeping after every single row keeps every row intact.
	 */
	ctx->count++;
	k_sleep(K_MSEC(10));

	return 0;
}

static void storage_dump_all(void)
{
	struct storage_dump_ctx ctx = { .count = 0 };
	int err;

	if (!storage_fcb_ok) {
		printk("# flash log not available (storage_fcb_init failed at boot)\n");
		return;
	}

	/* Grace period before any dump output starts, so there's a reliable
	 * window to get a capture tool attached after a reset/flash.
	 */
	for (int s = 5; s > 0; s--) {
		printk("# dump starting in %ds...\n", s);
		k_sleep(K_MSEC(1000));
	}

	printk("node_id,seq,flags,row,temp_c,humidity_pct,millis_since_init\n");

	err = fcb_walk(&storage_fcb, NULL, storage_dump_walk_cb, &ctx);
	if (err) {
		printk("# fcb_walk failed (err %d)\n", err);
		return;
	}

	printk("# %u rows\n", ctx.count);
}

/* ======================================================
 * MEASUREMENT PHASE: periodic sensor read + flash append. Completely
 * independent of BLE connection state -- once started (see
 * write_start_measuring() below), this keeps running via its own
 * self-reschedule forever, whether or not a central is anywhere nearby.
 * This independence is the entire point of the pivot away from PAwR (see
 * file header).
 * ====================================================== */

static void sensor_read_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sensor_read_work, sensor_read_work_handler);

static void sensor_read_work_handler(struct k_work *work)
{
	sensors_read(&latest_payload);
	storage_fcb_append(&latest_payload);

	APP_LOG("[SENSORS] node %u seq %u t+%ums temp=%d.%02uC humidity=%u.%u%% flags=0x%02x\n",
	       latest_payload.node_id, latest_payload.seq, latest_payload.millis_since_init,
	       latest_payload.temp_cdeg / 100, abs(latest_payload.temp_cdeg % 100),
	       latest_payload.humidity_pct10 / 10, latest_payload.humidity_pct10 % 10,
	       latest_payload.flags);

	k_work_schedule(&sensor_read_work, K_MSEC(SENSOR_READ_INTERVAL_MS));
}

/* ======================================================
 * DOWNLOAD PHASE: streams the entire flash log back to central as a
 * sequence of GATT indications, adapted from the backfill-ble-retrieval
 * branch's already-proven request/header/data/done design (that design's
 * blocker -- GATT indicate colliding with concurrent PAwR subevent polling
 * on the radio -- no longer applies, since there is no more PAwR at all).
 * Unlike that branch, there's no since-timestamp filtering: every download
 * sends the whole log (see struct download_req's comment in
 * pawr_protocol.h for why).
 * ====================================================== */

/* Declared here (rather than down by BT_GATT_SERVICE_DEFINE, where
 * pawr_svc_uuid/pawr_start_char_uuid live) since download_indicate_blocking()
 * below needs pawr_download_char_uuid to identify which characteristic's
 * value to indicate on.
 */
static const struct bt_uuid_128 pawr_download_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2));

static K_SEM_DEFINE(sem_download_indicate_confirmed, 0, 1);
static struct bt_gatt_indicate_params download_indicate_params;
static uint8_t download_indicate_buf[247]; /* sized for the largest MTU this
					     * project negotiates for -- a
					     * header, data, or done message
					     * always fits well inside this.
					     */

static void download_indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params,
				  uint8_t err)
{
	if (err) {
		APP_LOG("[DOWNLOAD] indicate failed/not confirmed (att err %d)\n", err);
	}

	k_sem_give(&sem_download_indicate_confirmed);
}

/* Sends one indication and blocks (this runs on the system work queue via
 * download_work, never on the BT RX callback context) until the peer
 * confirms it or a timeout elapses. Returns 0 on confirmed delivery,
 * negative errno otherwise.
 */
static int download_indicate_blocking(struct bt_conn *conn, const void *data, uint16_t len)
{
	int err;

	k_sem_reset(&sem_download_indicate_confirmed);

	memset(&download_indicate_params, 0, sizeof(download_indicate_params));
	download_indicate_params.attr = NULL;
	download_indicate_params.uuid = &pawr_download_char_uuid.uuid;
	download_indicate_params.func = download_indicate_cb;
	download_indicate_params.data = data;
	download_indicate_params.len = len;

	err = bt_gatt_indicate(conn, &download_indicate_params);
	if (err) {
		return err;
	}

	/* Generous per-PDU timeout -- no concurrent PAwR radio activity to
	 * contend with anymore (that was the backfill branch's actual
	 * blocker), so a confirm should land within a handful of connection
	 * events under any normal condition; 5s gives headroom without
	 * risking the whole transfer hanging forever on one stuck PDU.
	 */
	if (k_sem_take(&sem_download_indicate_confirmed, K_SECONDS(5))) {
		APP_LOG("[DOWNLOAD] indicate confirm timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

struct download_count_ctx {
	uint32_t count;
};

static int download_count_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
	struct download_count_ctx *ctx = arg;

	if (loc_ctx->loc.fe_data_len == sizeof(struct sensor_payload)) {
		ctx->count++;
	}

	return 0;
}

struct download_send_ctx {
	struct bt_conn        *conn;
	uint16_t                mtu_payload; /* usable bytes/indication, ATT
					       * header already subtracted
					       */
	uint8_t                 entries_per_pdu;
	struct sensor_payload   pending[20]; /* holds up to one PDU's worth
					       * before flushing
					       */
	uint8_t                 pending_count;
	uint16_t                total_sent;
	bool                    failed;
};

static bool download_flush_pending(struct download_send_ctx *ctx)
{
	struct download_data_pdu *pdu = (struct download_data_pdu *)download_indicate_buf;
	size_t pdu_len;

	if (ctx->pending_count == 0) {
		return true;
	}

	pdu->msg_type = DOWNLOAD_MSG_DATA;
	pdu->count = ctx->pending_count;
	memcpy(pdu->entries, ctx->pending, ctx->pending_count * sizeof(struct sensor_payload));
	pdu_len = offsetof(struct download_data_pdu, entries) +
		  ctx->pending_count * sizeof(struct sensor_payload);

	if (download_indicate_blocking(ctx->conn, download_indicate_buf, pdu_len)) {
		ctx->failed = true;
		return false;
	}

	ctx->total_sent += ctx->pending_count;
	ctx->pending_count = 0;

	return true;
}

static int download_send_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
	struct download_send_ctx *ctx = arg;
	struct sensor_payload payload;

	if (ctx->failed) {
		return 1; /* stop walking, a previous indicate already failed */
	}

	if (loc_ctx->loc.fe_data_len != sizeof(payload)) {
		return 0;
	}

	if (flash_area_read(loc_ctx->fap, FCB_ENTRY_FA_DATA_OFF(loc_ctx->loc), &payload,
			     sizeof(payload))) {
		return 0;
	}

	ctx->pending[ctx->pending_count++] = payload;

	if (ctx->pending_count >= ctx->entries_per_pdu) {
		if (!download_flush_pending(ctx)) {
			return 1;
		}
	}

	return 0;
}

/* 2026-08-13, found while root-causing a download-phase stall on real
 * hardware: Zephyr's BT subsystem requires the system workqueue to run at
 * a cooperative priority (see subsys/bluetooth/Kconfig's own comment on
 * SYSTEM_WORKQUEUE_PRIORITY), and download_work previously ran ON that
 * same system workqueue via the plain K_WORK_DEFINE below. A multi-second
 * transfer blocking there (waiting on indicate confirms) is exactly the
 * kind of long-running work Zephyr's own docs warn against parking on the
 * system workqueue, since it can delay other things that need that same
 * queue -- including, plausibly, the Bluetooth host's own processing.
 * Moved to a small dedicated work queue/thread instead, so this transfer's
 * blocking waits can never contend with the system workqueue or anything
 * else running on it.
 */
#define DOWNLOAD_WORKQ_STACK_SIZE 2048
#define DOWNLOAD_WORKQ_PRIORITY   K_LOWEST_APPLICATION_THREAD_PRIO

static K_THREAD_STACK_DEFINE(download_workq_stack, DOWNLOAD_WORKQ_STACK_SIZE);
static struct k_work_q download_workq;

static void download_work_handler(struct k_work *work);
static K_WORK_DEFINE(download_work, download_work_handler);

static void download_work_handler(struct k_work *work)
{
	/* Takes its own reference rather than trusting default_conn to stay
	 * valid for this work item's whole (potentially multi-second)
	 * lifetime -- disconnected() drops its own reference and can run
	 * concurrently with this deferred work, and without this the
	 * underlying bt_conn could be freed mid-transfer.
	 */
	struct bt_conn *conn = default_conn ? bt_conn_ref(default_conn) : NULL;
	struct download_count_ctx count_ctx = { 0 };
	struct download_send_ctx send_ctx = { 0 };
	struct download_header header;
	struct download_done done;
	uint16_t mtu;

	if (!conn) {
		APP_LOG("[DOWNLOAD] no active connection, dropping request\n");
		return;
	}

	if (!storage_fcb_ok) {
		APP_LOG("[DOWNLOAD] flash log unavailable, nothing to send\n");
		goto out;
	}

	fcb_walk(&storage_fcb, NULL, download_count_walk_cb, &count_ctx);

	mtu = bt_gatt_get_mtu(conn);

	header.msg_type = DOWNLOAD_MSG_HEADER;
	header.total_entries = (uint16_t)count_ctx.count;

	APP_LOG("[DOWNLOAD] sending %u entries\n", header.total_entries);

	if (download_indicate_blocking(conn, &header, sizeof(header))) {
		APP_LOG("[DOWNLOAD] header indicate failed, aborting transfer\n");
		goto out;
	}

	if (count_ctx.count == 0) {
		done.msg_type = DOWNLOAD_MSG_DONE;
		done.entries_sent = 0;
		download_indicate_blocking(conn, &done, sizeof(done));
		goto out;
	}

	/* mtu is the full negotiated ATT MTU; ATT itself reserves 3 bytes
	 * (opcode + handle) of any PDU, so usable payload is mtu - 3. Cap
	 * defensively at the fixed download_indicate_buf/pending[] sizing in
	 * case MTU somehow negotiated higher than expected.
	 */
	send_ctx.conn = conn;
	send_ctx.mtu_payload = MIN(mtu, sizeof(download_indicate_buf)) - 3;
	send_ctx.entries_per_pdu = MIN(
		(send_ctx.mtu_payload - offsetof(struct download_data_pdu, entries)) /
			sizeof(struct sensor_payload),
		ARRAY_SIZE(send_ctx.pending));

	if (send_ctx.entries_per_pdu == 0) {
		APP_LOG("[DOWNLOAD] negotiated MTU too small to carry even one entry, aborting\n");
		goto out;
	}

	fcb_walk(&storage_fcb, NULL, download_send_walk_cb, &send_ctx);

	if (!send_ctx.failed) {
		download_flush_pending(&send_ctx);
	}

	if (send_ctx.failed) {
		APP_LOG("[DOWNLOAD] transfer aborted after %u/%u entries (indicate failure)\n",
		       send_ctx.total_sent, header.total_entries);
		goto out;
	}

	done.msg_type = DOWNLOAD_MSG_DONE;
	done.entries_sent = send_ctx.total_sent;
	download_indicate_blocking(conn, &done, sizeof(done));

	APP_LOG("[DOWNLOAD] transfer complete: %u entries sent\n", send_ctx.total_sent);

out:
	bt_conn_unref(conn);
}

static ssize_t write_download_req(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(struct download_req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	APP_LOG("[DOWNLOAD] request received, submitting work item\n");

	/* Deferred to a dedicated work item, not handled inline here -- this
	 * callback runs in BT RX context, and a download transfer can take
	 * several seconds (many indicate-confirm round trips).
	 */
	k_work_submit_to_queue(&download_workq, &download_work);

	return len;
}

static void download_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	APP_LOG("[DOWNLOAD] indications %s\n", value == BT_GATT_CCC_INDICATE ? "enabled" : "disabled");
}

/* ======================================================
 * INIT PHASE: central writes this characteristic (empty payload) once, at
 * experiment start, to tell this node "you are synced, start measuring
 * now." See file header.
 * ====================================================== */

static ssize_t write_start_measuring(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (measuring) {
		/* Idempotent, not an error -- a retried/duplicate init write
		 * (e.g. central retrying after a dropped ack) must not reset
		 * t0 and silently discard everything measured so far.
		 */
		APP_LOG("[INIT] already measuring, ignoring repeat start-measuring write\n");
		return len;
	}

	t0_ms = k_uptime_get();
	measuring = true;
	gpio_pin_set_dt(&status_led, 1);

	APP_LOG("[INIT] synced, starting measurement (t0=%lld)\n", t0_ms);

	k_work_schedule(&sensor_read_work, K_NO_WAIT);

	return len;
}

static const struct bt_uuid_128 pawr_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
static const struct bt_uuid_128 pawr_start_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));

BT_GATT_SERVICE_DEFINE(pawr_svc, BT_GATT_PRIMARY_SERVICE(&pawr_svc_uuid.uuid),
		       BT_GATT_CHARACTERISTIC(&pawr_start_char_uuid.uuid, BT_GATT_CHRC_WRITE,
					      BT_GATT_PERM_WRITE, NULL, write_start_measuring,
					      NULL),
		       BT_GATT_CHARACTERISTIC(&pawr_download_char_uuid.uuid,
					      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,
					      BT_GATT_PERM_WRITE, NULL, write_download_req,
					      NULL),
		       BT_GATT_CCC(download_ccc_changed,
				   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

void connected(struct bt_conn *conn, uint8_t err)
{
	APP_LOG("Connected, err 0x%02X %s\n", err, bt_hci_err_to_str(err));

	if (err) {
		default_conn = NULL;

		return;
	}

	default_conn = bt_conn_ref(conn);
}

void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bt_conn_unref(default_conn);
	default_conn = NULL;

	APP_LOG("Disconnected, reason 0x%02X %s\n", reason, bt_hci_err_to_str(reason));

	k_sem_give(&sem_disconnected);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Advertised name is built at runtime (not CONFIG_BT_DEVICE_NAME directly)
 * so it can include CONFIG_APP_CENTRAL_ID as a suffix -- see
 * pawr_format_adv_name() in common/pawr_protocol.h for why/format.
 */
static char adv_name[PAWR_ADV_NAME_MAX_LEN];
static struct bt_data ad[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, 0), /* .data_len set in main() */
};

int main(void)
{
	int err;

	APP_LOG("Starting BLE GATT sensor node\n");
	APP_LOG("Node ID: %u, Central ID: %u\n", CONFIG_APP_NODE_ID, CONFIG_APP_CENTRAL_ID);

	pawr_format_adv_name(adv_name, sizeof(adv_name), CONFIG_APP_CENTRAL_ID, CONFIG_APP_NODE_ID);
	ad[0].data_len = strlen(adv_name);

	status_led_init();
	power_on_led_blink();
	sensors_init();
	storage_fcb_init();

	if (IS_ENABLED(CONFIG_APP_DUMP_ON_BOOT)) {
		storage_dump_all();
	}

	k_work_queue_init(&download_workq);
	k_work_queue_start(&download_workq, download_workq_stack,
			   K_THREAD_STACK_SIZEOF(download_workq_stack), DOWNLOAD_WORKQ_PRIORITY,
			   NULL);

	err = bt_enable(NULL);
	if (err) {
		APP_LOG("Bluetooth init failed (err %d)\n", err);

		return 0;
	}

	/* Advertise continuously, not just during a brief onboarding window
	 * -- this node needs to be connectable both for the init handshake
	 * and, much later, for the download phase, with an arbitrary amount
	 * of pure-measurement time in between where central isn't listening
	 * at all. sensor_read_work is deliberately NOT scheduled here (unlike
	 * the old PAwR design's "seed a first reading immediately") -- it
	 * only starts once write_start_measuring() actually runs, so a node
	 * that hasn't been through its init phase yet doesn't measure/store
	 * anything with a meaningless t0.
	 *
	 * Connectable advertising stops the instant a connection forms (it
	 * preallocates the single connection object this board has room for,
	 * see CONFIG_BT_MAX_CONN=1) and must be explicitly restarted after
	 * every disconnect -- this loop does that, forever, so the node stays
	 * reachable for however many connect/disconnect cycles happen over
	 * the node's whole lifetime (init once, then zero or more download
	 * attempts, with arbitrary measurement time in between). Sensor
	 * measurement itself (once started) never depends on this loop or on
	 * a connection existing -- it runs entirely on its own via
	 * sensor_read_work's self-reschedule, this is purely about staying
	 * connectable.
	 */
	while (true) {
		err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			APP_LOG("Advertising failed to start (err %d)\n", err);

			return 0;
		}

		APP_LOG("Advertising...\n");

		k_sem_take(&sem_disconnected, K_FOREVER);
		APP_LOG("Central disconnected -- still measuring/storing in the background, re-advertising\n");
	}

	return 0;
}
