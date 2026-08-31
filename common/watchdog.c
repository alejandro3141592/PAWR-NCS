/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Last-resort recovery for a central wedged by a bug in the onboarding
 * retry loop (2026-08-30, see device_found()'s bt_conn_le_create() failure
 * comment for the one real instance found and fixed the same day). This is
 * defense-in-depth against that *class* of bug, not a substitute for fixing
 * the actual cause when one is found -- see watchdog_feed()'s own comment
 * for why it must only be called from a point that proves real forward
 * progress, never from an unrelated periodic timer.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>

#include "watchdog.h"

#define WATCHDOG_NODE DT_ALIAS(watchdog0)

/* Generous relative to the main loop's own inner timeouts (up to ~2 *
 * PAWR_INTERVAL_MS for a sync wait, 10s for GATT discovery, 10s for a GATT
 * write -- worst case a single onboarding attempt takes on the order of
 * tens of seconds, not minutes) -- this window only needs to be long enough
 * to never fire during genuinely slow-but-still-working retries (a node
 * that's simply hard to reach shouldn't reboot the whole central), while
 * still being short enough that a real deployment doesn't sit silently
 * wedged for hours before self-recovering.
 */
#define WATCHDOG_TIMEOUT_MS (3 * 60 * 1000)

static const struct device *const wdt_dev = DEVICE_DT_GET(WATCHDOG_NODE);
static int wdt_channel_id = -1;

int watchdog_init(void)
{
	if (!device_is_ready(wdt_dev)) {
		printk("[WDT] Device not ready, hang-recovery watchdog disabled\n");
		return -ENODEV;
	}

	struct wdt_timeout_cfg wdt_config = {
		.window.min = 0,
		.window.max = WATCHDOG_TIMEOUT_MS,
		.callback = NULL, /* no warning callback -- reset only */
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_config);
	if (wdt_channel_id < 0) {
		printk("[WDT] Failed to install timeout (err %d), disabled\n", wdt_channel_id);
		return wdt_channel_id;
	}

	int err = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (err) {
		printk("[WDT] Failed to start (err %d), disabled\n", err);
		wdt_channel_id = -1;
		return err;
	}

	printk("[WDT] Armed, %d ms timeout -- resets if the main loop stops "
	       "making forward progress\n", WATCHDOG_TIMEOUT_MS);
	return 0;
}

void watchdog_feed(void)
{
	/* Only ever call this from a point that proves the main loop is
	 * genuinely still alive (e.g. a scan (re)start succeeding) -- NOT
	 * from a separate periodic timer/thread. A timer-based feed would
	 * keep resetting this countdown even while the main loop itself is
	 * completely wedged (the exact failure mode this exists to catch),
	 * making the watchdog useless for its one job.
	 */
	if (wdt_channel_id < 0) {
		return;
	}

	(void)wdt_feed(wdt_dev, wdt_channel_id);
}
