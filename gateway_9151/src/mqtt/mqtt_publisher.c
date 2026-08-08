/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Broker-agnostic MQTT publisher: works against a plain local Mosquitto
 * (APP_MQTT_USE_TLS=n, port 1883) or a TLS cloud broker like HiveMQ Cloud
 * (APP_MQTT_USE_TLS=y, port 8883), selected entirely by Kconfig (see
 * ../../Kconfig) -- no source change needed to switch. API usage confirmed
 * against C:/ncs/v3.3.0/zephyr/samples/net/mqtt_publisher/src/main.c, the
 * in-tree reference for zephyr/net/mqtt.h.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>

#include "mqtt_publisher.h"

#if defined(CONFIG_APP_MQTT_USE_TLS)
#include "tls_provision.h"
#endif

#define MQTT_RX_TX_BUFFER_SIZE 256
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_CONNECT_TRIES 5

static uint8_t rx_buffer[MQTT_RX_TX_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_RX_TX_BUFFER_SIZE];

static struct mqtt_client client_ctx;
static struct sockaddr_storage broker_addr;
static struct pollfd fds[1];
static int nfds;
static bool connected;

#if defined(CONFIG_APP_MQTT_USE_TLS)
/* sec_tag_list must outlive the connection (mqtt_sec_config just points at
 * it), so this can't be a function-local stack array.
 */
static sec_tag_t sec_tag_list[] = { APP_TLS_SEC_TAG };
#endif

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	ARG_UNUSED(client);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			printk("[MQTT] Connect failed: %d\n", evt->result);
			break;
		}
		connected = true;
		printk("[MQTT] Connected\n");
		break;

	case MQTT_EVT_DISCONNECT:
		printk("[MQTT] Disconnected: %d\n", evt->result);
		connected = false;
		nfds = 0;
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			printk("[MQTT] PUBACK error: %d\n", evt->result);
		}
		break;

	default:
		break;
	}
}

static int resolve_broker(struct sockaddr_storage *addr)
{
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *result;
	char port_str[6];

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_MQTT_BROKER_PORT);

	int err = getaddrinfo(CONFIG_APP_MQTT_BROKER_HOSTNAME, port_str, &hints, &result);

	if (err) {
		printk("[MQTT] getaddrinfo failed for %s: %d\n",
		       CONFIG_APP_MQTT_BROKER_HOSTNAME, err);
		return -EHOSTUNREACH;
	}

	memcpy(addr, result->ai_addr, result->ai_addrlen);
	freeaddrinfo(result);
	return 0;
}

static void client_init(struct mqtt_client *client)
{
	mqtt_client_init(client);

	client->broker = &broker_addr;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = (uint8_t *)CONFIG_APP_MQTT_CLIENT_ID;
	client->client_id.size = strlen(CONFIG_APP_MQTT_CLIENT_ID);
	client->protocol_version = MQTT_VERSION_3_1_1;

#if defined(CONFIG_APP_MQTT_USE_TLS)
	client->transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls_config = &client->transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = sec_tag_list;
	tls_config->sec_tag_count = ARRAY_SIZE(sec_tag_list);
	tls_config->hostname = CONFIG_APP_MQTT_BROKER_HOSTNAME;
#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

	if (strlen(CONFIG_APP_MQTT_USERNAME) > 0) {
		static struct mqtt_utf8 username = {
			.utf8 = (uint8_t *)CONFIG_APP_MQTT_USERNAME,
			.size = sizeof(CONFIG_APP_MQTT_USERNAME) - 1,
		};
		client->user_name = &username;
	}

	if (strlen(CONFIG_APP_MQTT_PASSWORD) > 0) {
		static struct mqtt_utf8 password = {
			.utf8 = (uint8_t *)CONFIG_APP_MQTT_PASSWORD,
			.size = sizeof(CONFIG_APP_MQTT_PASSWORD) - 1,
		};
		client->password = &password;
	}

	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);
}

static void prepare_fds(struct mqtt_client *client)
{
#if defined(CONFIG_APP_MQTT_USE_TLS)
	fds[0].fd = client->transport.tls.sock;
#else
	fds[0].fd = client->transport.tcp.sock;
#endif
	fds[0].events = POLLIN;
	nfds = 1;
}

static int wait_socket(int timeout_ms)
{
	if (nfds <= 0) {
		return 0;
	}
	return poll(fds, nfds, timeout_ms);
}

int mqtt_publisher_init(void)
{
	/* CONFIG_APP_MQTT_CLIENT_ID has no default (see Kconfig) specifically
	 * so this can't be silently skipped -- an empty client ID here means
	 * this board's secrets.conf never set one, and connecting anyway
	 * would risk colliding with another gateway's client ID on the same
	 * broker (see NOTES.md 2026-08-08 multi-gateway audit). Fail loudly
	 * at boot instead of connecting under a shared/blank ID.
	 */
	if (strlen(CONFIG_APP_MQTT_CLIENT_ID) == 0) {
		printk("[MQTT] FATAL: CONFIG_APP_MQTT_CLIENT_ID is not set -- add a "
		       "unique CONFIG_APP_MQTT_CLIENT_ID=\"...\" to this board's "
		       "secrets.conf before flashing. Refusing to connect with a "
		       "blank client ID.\n");
		k_panic();
	}

	int err = resolve_broker(&broker_addr);

	if (err) {
		return err;
	}

	for (int attempt = 0; attempt < MQTT_CONNECT_TRIES && !connected; attempt++) {
		client_init(&client_ctx);

		err = mqtt_connect(&client_ctx);
		if (err) {
			printk("[MQTT] mqtt_connect failed: %d, retrying\n", err);
			k_sleep(K_MSEC(1000));
			continue;
		}

		prepare_fds(&client_ctx);

		if (wait_socket(MQTT_CONNECT_TIMEOUT_MS) > 0) {
			mqtt_input(&client_ctx);
		}

		if (!connected) {
			mqtt_abort(&client_ctx);
		}
	}

	return connected ? 0 : -ETIMEDOUT;
}

#define SENSOR_PUBLISH_TOPIC "sensors/data"

/* Publishes the raw sensor_payload struct as-is (8 bytes, little-endian on
 * this target -- node_id, flags, seq, temp_cdeg, humidity_pct10, see
 * common/pawr_protocol.h) instead of JSON. Replaces the prior scheme of two
 * separate JSON-encoded publishes (sensors/temperature, sensors/humidity,
 * ~140 bytes of MQTT payload combined) with one ~8-byte binary publish --
 * roughly a 17x cut in application-layer bytes, and halves the fixed
 * per-message overhead (topic string, TLS record, TCP/IP framing) by
 * sending one message instead of two. Not human-readable via a generic
 * MQTT client/dashboard -- see gui/sensor_gui.py for the matching decode
 * (Python struct.unpack("<BBHhH", ...)). See NOTES.md 2026-08-04 for the
 * cellular data-usage motivation.
 */
int mqtt_publisher_send(const struct sensor_payload *payload)
{
	if (!connected) {
		return -ENOTCONN;
	}

	struct mqtt_publish_param param = { 0 };

	param.message.topic.topic.utf8 = (uint8_t *)SENSOR_PUBLISH_TOPIC;
	param.message.topic.topic.size = strlen(SENSOR_PUBLISH_TOPIC);
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = sizeof(*payload);
	param.message_id = sys_rand16_get();
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(&client_ctx, &param);
}

void mqtt_publisher_process(void)
{
	if (!connected) {
		return;
	}

	if (wait_socket(0) > 0) {
		mqtt_input(&client_ctx);
	}

	int err = mqtt_live(&client_ctx);

	if (err && err != -EAGAIN) {
		printk("[MQTT] mqtt_live error: %d\n", err);
	}
}
