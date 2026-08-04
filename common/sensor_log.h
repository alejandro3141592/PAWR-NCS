/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-board flash log (Flash Circular Buffer), shared by central and
 * gateway_9151: fallback local record of sensor_payload records that
 * couldn't yet (or didn't) make it to the next hop, so a multi-hour run has
 * a recoverable trail even through a UART/MQTT/LTE outage. Same design as
 * peripheral/src/main.c's storage_fcb_* (see that file's comments for the
 * full rationale) -- lifted out here so central and gateway_9151 don't each
 * reimplement the same FCB init/append/dump boilerplate.
 */

#ifndef SENSOR_LOG_H_
#define SENSOR_LOG_H_

#include <stdbool.h>
#include "pawr_protocol.h"

/* Opens (or creates) the board's "storage_partition" (Partition Manager
 * auto-reserves this on any board once CONFIG_FLASH_MAP=y) as an FCB log.
 * Safe to call even if it fails -- sensor_log_append() just silently no-ops
 * afterward, logging is a fallback, never a dependency for the primary data
 * path.
 */
void sensor_log_init(void);

/* Appends one payload to the flash log. Never blocks the caller on failure;
 * logs the error and returns.
 */
void sensor_log_append(const struct sensor_payload *payload);

/* Dumps the entire flash log as CSV over printk() (header row, one CSV row
 * per record, trailing "# N rows" line) -- same retrieval mechanism as
 * peripheral's CONFIG_APP_DUMP_ON_BOOT, capture via serial console.
 */
void sensor_log_dump_all(void);

#endif /* SENSOR_LOG_H_ */
