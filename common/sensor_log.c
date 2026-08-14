/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * See sensor_log.h. Ported from peripheral/src/main.c's storage_fcb_*
 * (proven on real hardware 2026-08-03) with no behavioral change other than
 * being reusable across apps -- see that file's comments for the full
 * rationale behind the erase-and-retry-on-foreign-data handling.
 */

#include <stdlib.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include "sensor_log.h"

#define STORAGE_FCB_SECTOR_MAX 8

static struct flash_sector storage_fcb_sectors[STORAGE_FCB_SECTOR_MAX];
static struct fcb storage_fcb;
static bool storage_fcb_ok;

void sensor_log_init(void)
{
	uint32_t sector_cnt = ARRAY_SIZE(storage_fcb_sectors);
	int err;

	err = flash_area_get_sectors(FIXED_PARTITION_ID(storage_partition), &sector_cnt,
				      storage_fcb_sectors);
	if (err) {
		printk("[STORAGE] Failed to get flash sectors (err %d)\n", err);
		return;
	}

	storage_fcb = (struct fcb){
		.f_magic = 0x50415752, /* "PAWR", arbitrary non-0xFFFFFFFF marker */
		.f_sector_cnt = sector_cnt,
		.f_sectors = storage_fcb_sectors,
	};

	err = fcb_init(FIXED_PARTITION_ID(storage_partition), &storage_fcb);
	if (err == -ENOMSG) {
		/* Sector header magic matched neither "erased" nor our own
		 * magic -- leftover data from something else. Standard FCB
		 * recovery: erase the whole partition once and retry.
		 */
		const struct flash_area *fap;

		printk("[STORAGE] Flash log area has foreign data, erasing and retrying\n");

		err = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fap);
		if (!err) {
			err = flash_area_erase(fap, 0, fap->fa_size);
			flash_area_close(fap);
		}

		if (err) {
			printk("[STORAGE] Failed to erase flash log area (err %d)\n", err);
			return;
		}

		err = fcb_init(FIXED_PARTITION_ID(storage_partition), &storage_fcb);
	}

	if (err) {
		printk("[STORAGE] Failed to init flash log (err %d)\n", err);
		return;
	}

	storage_fcb_ok = true;
	printk("[STORAGE] Flash log ready (%u sectors)\n", sector_cnt);
}

/* Proactive rotation: check/rotate BEFORE the log is actually full, so the
 * erase always has a full spare sector of headroom rather than ever
 * happening under write pressure. Proven on real hardware 2026-08-11 in
 * peripheral/src/main.c's identical storage_fcb_append() (see that file's
 * comment for the two earlier reactive-rotation attempts that caused
 * problems for reasons never fully root-caused) -- ported here 2026-08-14
 * after this file's own lack of any rotation logic let central's flash log
 * fill up permanently during real-hardware download testing: every
 * sensor_log_append() call failed with -ENOSPC from that point on, with no
 * way to recover short of a reflash. This file was never given the same
 * fix peripheral got, despite being explicitly flagged as a known gap
 * earlier in this project's history.
 */
#define STORAGE_FCB_ROTATE_FREE_SECTOR_THRESHOLD 2

void sensor_log_append(const struct sensor_payload *payload)
{
	struct fcb_entry loc;
	int err;

	if (!storage_fcb_ok) {
		return;
	}

	if (fcb_free_sector_cnt(&storage_fcb) <= STORAGE_FCB_ROTATE_FREE_SECTOR_THRESHOLD) {
		err = fcb_rotate(&storage_fcb);
		if (err) {
			printk("[STORAGE] fcb_rotate failed (err %d)\n", err);
		} else {
			printk("[STORAGE] fcb_rotate: log wrapped, oldest sector reclaimed\n");
		}
	}

	err = fcb_append(&storage_fcb, sizeof(*payload), &loc);
	if (err) {
		printk("[STORAGE] fcb_append failed (err %d)\n", err);
		return;
	}

	err = flash_area_write(storage_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), payload,
				sizeof(*payload));
	if (err) {
		printk("[STORAGE] flash_area_write failed (err %d)\n", err);
		return;
	}

	err = fcb_append_finish(&storage_fcb, &loc);
	if (err) {
		printk("[STORAGE] fcb_append_finish failed (err %d)\n", err);
	}
}

struct storage_dump_ctx {
	uint32_t count;
};

static int storage_dump_walk_cb(struct fcb_entry_ctx *loc_ctx, void *arg)
{
	struct storage_dump_ctx *ctx = arg;
	struct sensor_payload payload;
	int err;

	if (loc_ctx->loc.fe_data_len != sizeof(payload)) {
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

	ctx->count++;

	return 0;
}

void sensor_log_dump_all(void)
{
	struct storage_dump_ctx ctx = { .count = 0 };
	int err;

	if (!storage_fcb_ok) {
		printk("# flash log not available (sensor_log_init failed at boot)\n");
		return;
	}

	printk("node_id,seq,flags,row,temp_c,humidity_pct,millis_since_init\n");

	err = fcb_walk(&storage_fcb, NULL, storage_dump_walk_cb, &ctx);
	if (err) {
		printk("# fcb_walk failed (err %d)\n", err);
		return;
	}

	printk("# %u rows\n", ctx.count);
}
