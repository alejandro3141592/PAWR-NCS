/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware watchdog, last-resort recovery if the main onboarding loop ever
 * gets wedged again (2026-08-30) -- see watchdog.c for the full rationale.
 */

#ifndef WATCHDOG_H_
#define WATCHDOG_H_

/* Installs and arms the hardware watchdog. Non-fatal if this fails (e.g. no
 * watchdog device on this board) -- watchdog_feed() below just no-ops in
 * that case, same pattern as this project's other optional subsystems
 * (gateway_uart_tx_init(), gui_uart_tx_init()). Call once at startup, before
 * the main loop.
 */
int watchdog_init(void);

/* Resets the watchdog countdown. Call ONLY from a point in the main loop
 * that proves real forward progress is still happening -- see this file's
 * top comment and watchdog.c's watchdog_feed() for why a periodic-timer
 * feed would defeat the whole point.
 */
void watchdog_feed(void);

#endif /* WATCHDOG_H_ */
