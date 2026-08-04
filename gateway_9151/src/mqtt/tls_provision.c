/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provisions the broker's CA certificate into the nRF9151 modem's own
 * credential storage. This project uses CONFIG_NET_SOCKETS_OFFLOAD=y (see
 * ../../prj.conf), meaning TLS sockets are handled by the modem firmware
 * itself, not Zephyr's own TLS stack -- so the credential API is
 * modem_key_mgmt_write() (AT%CMNG under the hood), not Zephyr's
 * tls_credential_add(). Confirmed against the in-tree reference
 * C:/ncs/v3.3.0/nrf/samples/net/mqtt/src/modules/transport/
 * credentials_provision/credentials_provision.c.
 *
 * Hard constraint, from modem_key_mgmt_write()'s own doc comment
 * (modem/modem_key_mgmt.h): "-EPERM: Not permitted when the LTE link is
 * active." So this must run after nrf_modem_lib initializes but BEFORE any
 * LTE connection attempt -- NRF_MODEM_LIB_ON_INIT is the right hook for
 * that (fires once, right after modem init, before main() has a chance to
 * bring the link up), not something called later from mqtt_publisher_init().
 */

#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/sys/printk.h>

#include "tls_provision.h"
#include "ca_cert.h"

static void on_modem_lib_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret != 0) {
		printk("[TLS] Modem library did not initialize: %d\n", ret);
		return;
	}

	bool exists = false;
	int err = modem_key_mgmt_exists(APP_TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					 &exists);

	if (err) {
		printk("[TLS] modem_key_mgmt_exists failed: %d\n", err);
		return;
	}

	if (exists) {
		printk("[TLS] CA cert already provisioned at sec_tag %d\n", APP_TLS_SEC_TAG);
		return;
	}

	err = modem_key_mgmt_write(APP_TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   ca_certificate, sizeof(ca_certificate) - 1);
	if (err) {
		printk("[TLS] modem_key_mgmt_write failed: %d\n", err);
		return;
	}

	printk("[TLS] CA cert provisioned at sec_tag %d\n", APP_TLS_SEC_TAG);
}

NRF_MODEM_LIB_ON_INIT(gateway_tls_provision_hook, on_modem_lib_init, NULL);
