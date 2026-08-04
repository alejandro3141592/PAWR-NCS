/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TLS_PROVISION_H_
#define TLS_PROVISION_H_

/* The sec_tag both this provisioning step and mqtt_publisher.c's TLS config
 * must agree on -- arbitrary but must be consistent and not collide with
 * any other credential this device provisions for a different purpose.
 */
#define APP_TLS_SEC_TAG 42

#endif /* TLS_PROVISION_H_ */
