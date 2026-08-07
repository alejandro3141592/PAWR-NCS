/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PAwR peripheral / sync+responder: skin sensor node.
 * Adapted from NCS sample: samples/bluetooth/periodic_sync_rsp
 *
 * Flow: connectable-advertise as "PAwR sync sample" -> central connects and
 * transfers periodic sync info (PAST) -> this device syncs to the central's
 * periodic advertising -> central writes our subevent/response-slot
 * assignment over GATT (kept as the extension point for future dynamic
 * slot-shifting) -> every 10 seconds we read skin temperature + humidity,
 * and answer every subevent poll for our assigned slot with the latest
 * reading.
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
 * development, but should be set to n for real deployment (50 unattended
 * field nodes) to avoid any chance of the serial console -- a USB-CDC
 * transport already shown this session to be fragile under load (see the
 * earlier "udc: Failed to allocate net_buf" hang, and the CONFIG_SHELL boot
 * hang) -- being a source of problems at all. IS_ENABLED() makes the
 * disabled branch dead code eliminated at compile time, not a runtime
 * check, so this has zero cost when off. See NOTES.md 2026-08-03.
 */
#define APP_LOG(fmt, ...)                                                                        \
	do {                                                                                       \
		if (IS_ENABLED(CONFIG_APP_SERIAL_LOGGING)) {                                      \
			printk(fmt, ##__VA_ARGS__);                                               \
		}                                                                                  \
	} while (0)

#define NAME_LEN 30

static K_SEM_DEFINE(sem_per_adv, 0, 1);
static K_SEM_DEFINE(sem_per_sync, 0, 1);
static K_SEM_DEFINE(sem_per_sync_lost, 0, 1);
static K_SEM_DEFINE(sem_disconnected, 0, 1);

static struct bt_conn *default_conn;
static struct bt_le_per_adv_sync *default_sync;
static struct __packed {
	uint8_t subevent;
	uint8_t response_slot;

} pawr_timing;

/* Status LED: off = not synced, steady on = synced (idle), brief off-blip =
 * a response was just sent for a subevent poll. The blip is scheduled as
 * delayed work rather than a blocking sleep since recv_cb runs in BT RX
 * context and must return promptly.
 */
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void status_led_blip_end(struct k_work *work)
{
	if (default_sync) {
		gpio_pin_set_dt(&status_led, 1);
	}
}

static K_WORK_DELAYABLE_DEFINE(status_led_blip_work, status_led_blip_end);

static void status_led_blip(void)
{
	gpio_pin_set_dt(&status_led, 0);
	k_work_reschedule(&status_led_blip_work, K_MSEC(100));
}

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
 * distinct from status_led (red, led0) above, which tracks PAwR sync state
 * throughout the rest of run time. Blocking sleep is fine here: this runs
 * once in main(), before Bluetooth/sensors start, not in a latency-sensitive
 * callback like status_led_blip().
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
		 * the PAwR data path without any I2C hardware attached.
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
}

/* ======================================================
 * ON-BOARD FLASH LOG (fallback storage for long unattended runs)
 *
 * The board's internal "Storage" devicetree partition (32KB, separate from
 * application code -- see nrf52840_partition_uf2_sdv7.dtsi) is used as a
 * Flash Circular Buffer: every sensor_payload produced is appended here as
 * well as sent over the air, so a multi-hour run has a complete local
 * record even if central misses some over-the-air responses (PAwR gives
 * the peripheral no delivery acknowledgment, so there's no way to log only
 * the ones that failed -- see NOTES.md 2026-08-03). At the current 10s
 * interval, a full 4-hour run is ~1440 records * 8 bytes = ~11.2KB, well
 * under the 32KB partition -- no wraparound expected in normal use, but if
 * the buffer does fill, FCB's circular behavior means oldest records are
 * overwritten first, not that appends start failing.
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
		 * area (confirmed on real hardware 2026-08-03: this is the
		 * first time this partition has ever been written by this
		 * project). Standard FCB recovery: erase the whole partition
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

/* Appends one payload to the flash log. Failure here is logged but never
 * blocks reporting over the air -- flash logging is a fallback, not a
 * dependency for the primary PAwR data path.
 */
static void storage_fcb_append(const struct sensor_payload *payload)
{
	struct fcb_entry loc;
	int err;

	if (!storage_fcb_ok) {
		return;
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

/* Retrieval mechanism (replaces the earlier shell-based "dump" command,
 * dropped 2026-08-03 after CONFIG_SHELL hung the board's console completely
 * -- see NOTES.md). Prints the whole flash log as CSV over the plain
 * printk() console instead, which is the same path already proven reliable
 * all session, no extra subsystem needed. Gated by CONFIG_APP_DUMP_ON_BOOT:
 * build with that set, flash the specific board whose log you want, and
 * capture its serial output right after boot (tools/Watch-SerialLog.ps1) --
 * the CSV is between the header row and the trailing "# N rows" line.
 * Deliberately unconditional on CONFIG_APP_SERIAL_LOGGING (the "quiet
 * production" toggle just below): a dump-mode build is a distinct,
 * intentional retrieval session, not something that should go silent
 * because the quiet flag happened to be left on from a production build.
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

	printk("%u,%u,0x%02x,%u,%d.%02u,%u.%u\n", payload.node_id, payload.seq, payload.flags,
	       ctx->count, payload.temp_cdeg / 100, abs(payload.temp_cdeg % 100),
	       payload.humidity_pct10 / 10, payload.humidity_pct10 % 10);

	ctx->count++;

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

	printk("node_id,seq,flags,row,temp_c,humidity_pct\n");

	err = fcb_walk(&storage_fcb, NULL, storage_dump_walk_cb, &ctx);
	if (err) {
		printk("# fcb_walk failed (err %d)\n", err);
		return;
	}

	printk("# %u rows\n", ctx.count);
}

static void sensor_read_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sensor_read_work, sensor_read_work_handler);

static void sensor_read_work_handler(struct k_work *work)
{
	sensors_read(&latest_payload);
	storage_fcb_append(&latest_payload);

	APP_LOG("[SENSORS] node %u seq %u temp=%d.%02uC humidity=%u.%u%% flags=0x%02x\n",
	       latest_payload.node_id, latest_payload.seq,
	       latest_payload.temp_cdeg / 100, abs(latest_payload.temp_cdeg % 100),
	       latest_payload.humidity_pct10 / 10, latest_payload.humidity_pct10 % 10,
	       latest_payload.flags);

	k_work_schedule(&sensor_read_work, K_MSEC(PAWR_INTERVAL_MS));
}

/* ======================================================
 * PAwR sync + response
 * ====================================================== */

static void sync_cb(struct bt_le_per_adv_sync *sync, struct bt_le_per_adv_sync_synced_info *info)
{
	struct bt_le_per_adv_sync_subevent_params params;
	uint8_t subevents[1];
	char le_addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));
	APP_LOG("Synced to %s with %d subevents\n", le_addr, info->num_subevents);

	default_sync = sync;

	params.properties = 0;
	params.num_subevents = 1;
	params.subevents = subevents;
	subevents[0] = pawr_timing.subevent;

	err = bt_le_per_adv_sync_subevent(sync, &params);
	if (err) {
		APP_LOG("Failed to set subevents to sync to (err %d)\n", err);
	} else {
		APP_LOG("Changed sync to subevent %d\n", subevents[0]);
	}

	gpio_pin_set_dt(&status_led, 1);

	k_sem_give(&sem_per_sync);
}

static void term_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	APP_LOG("Sync terminated (reason %d)\n", info->reason);

	default_sync = NULL;

	gpio_pin_set_dt(&status_led, 0);

	k_sem_give(&sem_per_sync_lost);
}

static struct bt_le_per_adv_response_params rsp_params;

NET_BUF_SIMPLE_DEFINE_STATIC(rsp_buf, 247);

static void recv_cb(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_recv_info *info, struct net_buf_simple *buf)
{
	int err;

	if (buf) {
		static const uint16_t company_id = 0xFFFF;

		APP_LOG(">>> Poll received: subevent %d, responding in slot %d\n", info->subevent,
		       pawr_timing.response_slot);

		net_buf_simple_reset(&rsp_buf);
		net_buf_simple_add_u8(&rsp_buf, 1 + 2 + sizeof(latest_payload));
		net_buf_simple_add_u8(&rsp_buf, BT_DATA_MANUFACTURER_DATA);
		net_buf_simple_add_le16(&rsp_buf, company_id);
		net_buf_simple_add_mem(&rsp_buf, &latest_payload, sizeof(latest_payload));

		rsp_params.request_event = info->periodic_event_counter;
		rsp_params.request_subevent = info->subevent;
		/* Respond in current subevent and assigned response slot */
		rsp_params.response_subevent = info->subevent;
		rsp_params.response_slot = pawr_timing.response_slot;

		err = bt_le_per_adv_set_response_data(sync, &rsp_params, &rsp_buf);
		if (err) {
			APP_LOG("Failed to send response (err %d)\n", err);
		} else {
			status_led_blip();
		}
	} else {
		APP_LOG("Failed to receive indication: subevent %d\n", info->subevent);
	}
}

static struct bt_le_per_adv_sync_cb sync_callbacks = {
	.synced = sync_cb,
	.term = term_cb,
	.recv = recv_cb,
};

static const struct bt_uuid_128 pawr_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
static const struct bt_uuid_128 pawr_char_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));

static ssize_t write_timing(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			    uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(pawr_timing)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(&pawr_timing, buf, len);

	APP_LOG("New timing: subevent %d, response slot %d\n", pawr_timing.subevent,
	       pawr_timing.response_slot);

	struct bt_le_per_adv_sync_subevent_params params;
	uint8_t subevents[1];
	int err;

	params.properties = 0;
	params.num_subevents = 1;
	params.subevents = subevents;
	subevents[0] = pawr_timing.subevent;

	if (default_sync) {
		err = bt_le_per_adv_sync_subevent(default_sync, &params);
		if (err) {
			APP_LOG("Failed to set subevents to sync to (err %d)\n", err);
		} else {
			APP_LOG("Changed sync to subevent %d\n", subevents[0]);
		}
	} else {
		APP_LOG("Not synced yet\n");
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(pawr_svc, BT_GATT_PRIMARY_SERVICE(&pawr_svc_uuid.uuid),
		       BT_GATT_CHARACTERISTIC(&pawr_char_uuid.uuid, BT_GATT_CHRC_WRITE,
					      BT_GATT_PERM_WRITE, NULL, write_timing,
					      &pawr_timing));

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
	struct bt_le_per_adv_sync_transfer_param past_param;
	int err;

	APP_LOG("Starting Periodic Advertising with Responses Synchronization Demo (peripheral)\n");
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

	err = bt_enable(NULL);
	if (err) {
		APP_LOG("Bluetooth init failed (err %d)\n", err);

		return 0;
	}

	bt_le_per_adv_sync_cb_register(&sync_callbacks);

	/* skip=0: with a 10s periodic interval, skipping even one event
	 * before the first sync attempt adds a full extra interval of
	 * latency to onboarding for no benefit (the demo's skip=1 made
	 * sense at its much shorter interval, not here).
	 */
	past_param.skip = 0;
	past_param.timeout = PAWR_PAST_TIMEOUT_UNITS;
	past_param.options = BT_LE_PER_ADV_SYNC_TRANSFER_OPT_NONE;
	err = bt_le_per_adv_sync_transfer_subscribe(NULL, &past_param);
	if (err) {
		APP_LOG("PAST subscribe failed (err %d)\n", err);

		return 0;
	}

	/* Seed a first reading immediately so the earliest subevent polls
	 * (before the first 10s tick) have something valid to respond with.
	 */
	sensors_read(&latest_payload);
	k_work_schedule(&sensor_read_work, K_MSEC(PAWR_INTERVAL_MS));

	do {
		/* If central is still connected from a previous attempt (e.g.
		 * the previous sync wait timed out while central was still
		 * mid-onboarding), wait for it to actually disconnect first --
		 * retrying connectable advertising while a connection is
		 * still live fails with -ENOMEM (CONFIG_BT_MAX_CONN=1 leaves
		 * no headroom for a second connection object). Drain any
		 * stale "already disconnected" signal first so a disconnect
		 * that happened earlier (while we were still waiting on
		 * sem_per_sync) can't leave the semaphore's count sitting at
		 * 1 and cause a *future* wait here to return instantly
		 * without an actual disconnect having happened yet.
		 */
		k_sem_take(&sem_disconnected, K_NO_WAIT);
		if (default_conn) {
			k_sem_take(&sem_disconnected, K_FOREVER);
		}

		err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err && err != -EALREADY) {
			APP_LOG("Advertising failed to start (err %d)\n", err);

			return 0;
		}

		APP_LOG("Waiting for periodic sync...\n");
		/* Central connects, sends PAST, discovers, writes the
		 * assignment, then deliberately holds the connection open a
		 * bit past one full periodic advertising interval before
		 * disconnecting (see central's post-write delay) so sync has
		 * time to land. That alone can take close to
		 * PAWR_INTERVAL_MS; add real margin here too so this wait
		 * doesn't expire first and race a retry against the still-live
		 * connection.
		 */
		err = k_sem_take(&sem_per_sync, K_MSEC(PAWR_INTERVAL_MS * 2));
		if (err) {
			APP_LOG("Timed out while synchronizing\n");

			continue;
		}

		APP_LOG("Periodic sync established.\n");

		err = k_sem_take(&sem_per_sync_lost, K_FOREVER);
		if (err) {
			APP_LOG("failed (err %d)\n", err);

			return 0;
		}

		APP_LOG("Periodic sync lost.\n");
	} while (true);

	return 0;
}
