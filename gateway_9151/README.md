# PAwR nRF9151 gateway

Bridges the PAwR `central` board (Seeed XIAO nRF52840) to an MQTT broker over
LTE-M/NB-IoT, so sensor readings collected over BLE reach a server off the
local network. Not built with PlatformIO -- this is an NCS/Zephyr `west` app,
sibling to `../central` and `../peripheral`.

```
central (XIAO nRF52840)  --UART1, 115200, framed+CRC16-->  gateway_9151 (nRF9151 DK)  --MQTT/TLS over LTE-->  HiveMQ Cloud
```

**Status (2026-08-04): confirmed working end-to-end on real hardware.**
LTE connects, TLS handshake to HiveMQ Cloud completes, MQTT connects, and the
UART link to `central` receives real sensor frames. See `../BUILD_AND_FLASH.md`
for build/flash commands.

## Required one-time hardware setup: disable VCOM1 on the DK

**Before the UART link to `central` will work at all**, the nRF9151 DK's
Arduino header UART (`arduino_serial`/`uart1`, used for this link) must be
released from the board's Interface MCU (IMCU):

1. Open **nRF Connect for Desktop** -> **Board Configurator** app (install
   it if not already added).
2. Connect to the nRF9151 DK.
3. Find the **VCOM1** setting and **disable/disconnect** it. Leave **VCOM0**
   connected -- that's the console (`uart0`) this app's `printk` output uses.
4. Apply/write the configuration, then power-cycle the board.

**Why:** the DK routes its Arduino header UART pins (TxD2/RxD2, P0.29/P0.28)
through the IMCU as a virtual COM port by default. Per Nordic's own
documentation: *"When working with nrf9151dk board with an external MCU
host, you must disable VCOM0 and VCOM1 in the Board Configurator app to
release the UART pins for external use."* Without this, the SiP's UARTE1
peripheral never sees any RX data at all -- confirmed the hard way over a
multi-session investigation (see `../NOTES.md` 2026-08-03/04): correct
devicetree/pinctrl/IRQ config, confirmed wire continuity, confirmed TX
toggling, and even a from-scratch polled-mode (`uart_poll_in`) self-test all
showed zero bytes ever received, on a UART with its own TX wired directly to
its own RX. The fix was entirely on the DK hardware-config side, not code --
no amount of firmware changes would have solved this. This isn't a
physical switch on the board (a routing switch was checked and ruled out
early in the investigation) -- it's IMCU firmware state set via that
separate app.

## Architecture

- `src/uart/uart_receiver.c` -- interrupt-driven UART RX, byte-at-a-time
  framing state machine, CRC-16 validation, hands decoded `sensor_payload`
  structs to a callback.
- `src/mqtt/mqtt_publisher.c` -- generic Zephyr MQTT client (`zephyr/net/mqtt.h`,
  broker-agnostic -- not nRF Cloud's MQTT library), resolves the broker
  hostname via `getaddrinfo`, connects (plain or TLS, see below), publishes
  one JSON message per sensor field (`sensors/temperature`,
  `sensors/humidity`).
- `src/mqtt/tls_provision.c` -- when `CONFIG_APP_MQTT_USE_TLS=y`, provisions
  the broker's CA cert into the **nRF9151 modem's own** credential storage
  via `modem_key_mgmt_write()` (AT%CMNG), not Zephyr's `tls_credential_add()`
  -- this project uses `CONFIG_NET_SOCKETS_OFFLOAD=y`, so TLS is handled by
  the modem directly. Hooked via `NRF_MODEM_LIB_ON_INIT`, which fires right
  after the modem library initializes and before any LTE connection attempt
  -- required, since `modem_key_mgmt_write()` returns `-EPERM` once the LTE
  link is active.
- `src/mqtt/ca_cert.h` -- embeds ISRG Root X1 (Let's Encrypt's root CA),
  fetched directly from `letsencrypt.org` and fingerprint-checked against
  the actual live TLS chain HiveMQ Cloud presents.
- `src/main.c` -- init order is deliberate: UART receiver first, then
  `nrf_modem_lib_init()`, then `lte_lc_connect_async()` (not Connection
  Manager -- dropped after comparing against the known-working
  `../tempUART_READER` reference project), then MQTT connect, then the main
  loop drains a `k_msgq` of decoded frames and publishes each one.
- `../common/uart_frame.h/.c` -- shared wire framing (`start byte + 8-byte
  sensor_payload + CRC16`), used by both this app and `central`'s
  `gateway_uart_tx.c`, so the two ends can't drift on frame format.
- `Kconfig` -- broker hostname/port/TLS-on-off/username/password/client ID,
  all switchable at build time without touching source -- same firmware
  image can point at a local Mosquitto (port 1883, no TLS) or HiveMQ Cloud
  (port 8883, TLS) by changing Kconfig values, via `secrets.conf` (see
  `../BUILD_AND_FLASH.md`).

## Known Kconfig gotchas already resolved (kept here in case anything regresses)

- `boards/nrf9151dk_nrf9151_ns.overlay` must re-enable `uart1`
  (`&uart1 { status = "okay"; };`) -- the `/ns` (non-secure) board variant
  disables it by default ("used by default in TF-M").
- `CONFIG_POSIX_API=y` is required for `mqtt_publisher.c`'s plain POSIX
  socket names (`struct addrinfo`, `POLLIN`, `getaddrinfo()`, `poll()`) --
  without it, `<zephyr/net/socket.h>` only exposes `zsock_`-prefixed forms.
  Also needs explicit `#include <zephyr/posix/poll.h>` and
  `<zephyr/posix/netdb.h>`.
- `APP_MQTT_USE_TLS` (in `Kconfig`) must `select MQTT_LIB_TLS` (otherwise
  `MQTT_TRANSPORT_SECURE`/`.transport.tls` are compiled out) and
  `select MODEM_KEY_MGMT` (`depends on NRF_MODEM_LIB` but not
  auto-selected -- needed for `modem_key_mgmt_write`/`_exists`).
  Deliberately does *not* select `CONFIG_MBEDTLS` -- wrong TLS stack when
  sockets are offloaded to the modem.

## Not yet done

1. **Local Mosquitto broker reachability is still an open, separate
   question** -- the 9151 reaches the internet over cellular, not your
   LAN/WiFi, so a Mosquitto instance needs to be reachable from the public
   internet (port-forward + auth, a tunnel, or similar) for that path to
   work at all. Not blocking the HiveMQ path above; revisit as its own task
   when you're ready to decide how to expose it securely.
2. **Message queue sizing** (`K_MSGQ_DEFINE(frame_msgq, ..., 8, 4)` in
   `src/main.c`) is a rough guess (8 slots), not measured -- revisit once
   real traffic (up to 17 nodes/10s cadence) is flowing and it's clear
   whether frames are ever actually dropped for queue-full reasons.
3. **Not yet soak-tested.** Confirmed working over short sessions; hasn't
   been run for hours at a time the way `central`/`peripheral` have.

## Building

See `../BUILD_AND_FLASH.md` for ready-to-paste commands for every app in
this repo, including this one.
