/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF9151 gateway: receives sensor_payload frames over UART from the PAwR
 * central board and republishes each as MQTT over LTE. See
 * gateway_9151/README.md for what's wired up vs. still a stub.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>

#include "pawr_protocol.h"
#include "sensor_log.h"
#include "uart/uart_receiver.h"
#include "mqtt/mqtt_publisher.h"

/* Init order (UART receiver, then modem lib, then LTE connect via
 * lte_lc_connect_async -- not Connection Manager) matches the confirmed-
 * working ../tempUART_READER reference project. See NOTES.md 2026-08-03/04:
 * the real bug that made the gateway's UART never receive anything was a
 * DK hardware setting (VCOM1 routing the Arduino header UART through the
 * board's Interface MCU instead of the SiP by default -- fixed via nRF
 * Connect for Desktop's Board Configurator app, not code), but this init
 * order is still worth keeping since it's the proven-working sequence.
 */
static K_SEM_DEFINE(lte_connected_sem, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			printk("[NET] LTE connected (%s)\n",
			       evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "home"
										     : "roaming");
			k_sem_give(&lte_connected_sem);
		}
		break;
	default:
		break;
	}
}

/* Queue of decoded frames between the UART ISR/callback context and the
 * main loop, which owns the (blocking) MQTT socket calls. A single-slot
 * message queue is enough at this sensor cadence (1 frame per node per 10s,
 * <=20 nodes -- worst case one frame every ~500ms, MQTT publish is much
 * faster than that), but if bursts start getting dropped, this is the first
 * thing to size up (see gateway_9151/README.md).
 */
K_MSGQ_DEFINE(frame_msgq, sizeof(struct sensor_payload), 8, 4);

static void on_uart_frame(const struct sensor_payload *payload)
{
	printk("[MAIN] UART frame received: node=%u seq=%u flags=0x%02x temp_cdeg=%d humidity_pct10=%u\n",
	       payload->node_id, payload->seq, payload->flags, payload->temp_cdeg,
	       payload->humidity_pct10);

	/* Fallback local record of every frame received from central, in case
	 * this gateway's own MQTT/LTE hop is down for a while -- same
	 * mechanism as peripheral's and central's on-board flash log, see
	 * common/sensor_log.h. Logged here (on receipt), not after a
	 * successful/failed publish, so the record is complete regardless of
	 * what happens downstream.
	 */
	sensor_log_append(payload);

	int err = k_msgq_put(&frame_msgq, payload, K_NO_WAIT);

	if (err) {
		printk("[MAIN] Frame queue full, dropping (node %u seq %u)\n",
		       payload->node_id, payload->seq);
	}
}

int main(void)
{
	printk("[MAIN] PAwR nRF9151 gateway starting\n");

	sensor_log_init();

	if (IS_ENABLED(CONFIG_APP_DUMP_LOG_ON_BOOT)) {
		sensor_log_dump_all();
	}

	int err = uart_receiver_init(on_uart_frame);

	if (err) {
		printk("[MAIN] uart_receiver_init failed: %d\n", err);
		return 0;
	}

	err = nrf_modem_lib_init();
	if (err) {
		printk("[MAIN] nrf_modem_lib_init failed: %d\n", err);
		return 0;
	}

	printk("[NET] Connecting to LTE...\n");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		printk("[MAIN] lte_lc_connect_async failed: %d\n", err);
		return 0;
	}

	k_sem_take(&lte_connected_sem, K_FOREVER);

	err = mqtt_publisher_init();
	if (err) {
		printk("[MAIN] mqtt_publisher_init failed: %d\n", err);
		return 0;
	}

	struct sensor_payload payload;

	while (1) {
		if (k_msgq_get(&frame_msgq, &payload, K_MSEC(500)) == 0) {
			err = mqtt_publisher_send(&payload);
			if (err) {
				printk("[MAIN] mqtt_publisher_send failed: %d (node %u seq %u)\n",
				       err, payload.node_id, payload.seq);
			}
		}

		mqtt_publisher_process();
	}

	return 0;
}
