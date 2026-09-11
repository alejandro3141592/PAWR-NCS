# broadcasting/ — Broadcasting architecture baseline

This is the original Broadcasting architecture (independent, connectionless
BLE advertising per node, passive-scanning central), ported into `PAwR-ncs`
on 2026-08-20 as the third arm of a thesis comparison against PAwR
(`central/`+`peripheral/`) and connection-oriented GATT store-and-forward
(the current `central/`+`peripheral/` architecture on `ble-gatt-store-forward`).
See `THESIS_TECHNICAL_REPORT.md` §6 for the full comparison context.

**Different toolchain, on purpose.** Everything else in this repo builds via
`west`/nRF Connect SDK (Zephyr). This folder is a standalone
**PlatformIO/Arduino** project, because Broadcasting's whole point of
comparison *is* the original pre-PAwR stack (Nordic SoftDevice S140 via the
Adafruit nRF52 Arduino core/Bluefruit), not a Zephyr reimplementation of the
same idea — see `../PAwR/does-pawr-is-supported-idempotent-origami.md` for
why that stack can't do PAwR at all (SoftDevice S140 predates Bluetooth 5.4).

## Provenance

Ported as close to verbatim as possible from a separate, standalone
repository (`~/Documents/Wearable`, branch `NewVersion`) — the actual
original Broadcasting implementation this whole project's PAwR migration
started from, referenced (but not included) by
`../PAwR/does-pawr-is-supported-idempotent-origami.md`'s feasibility
analysis. Kept deliberately close to a literal copy (same file/variable
names and structure) rather than restyled to match this repo's Zephyr code
conventions, since the thesis comparison needs the actual historical
architecture, not a reimplementation.

One real fix was needed to make it build standalone here: `platformio.ini`'s
`lib_deps` didn't declare the SparkFun MAX3010x library that
`lib/sensors/MAX30102Sensor/` depends on (`#include <MAX30105.h>`) — the
original project's local `.pio/libdeps` had it installed manually, outside
`lib_deps`, so a fresh checkout of the original wouldn't actually build
without that same manual step. Added `sparkfun/SparkFun MAX3010x Pulse and
Proximity Sensor Library` to `lib_deps` here to fix that gap.

**2026-08-20: wire payload changed to match PAwR-ncs exactly.** For a fair
thesis comparison, `AdvChannelPayload` (`lib/config/DataTypes.h`) was
changed from its original 18-byte shape (float temp/humidity, a `millis()`
timestamp, and an app-level CRC-16) to be byte-for-byte identical to
`common/pawr_protocol.h`'s original 8-byte PAwR-era `struct sensor_payload`
(`node_id`, `flags`, `seq`, `temp_cdeg` as centi-degrees, `humidity_pct10`
as tenths-of-a-percent) — not the current 16-byte GATT-era version, which
carries `millis_since_init`/`init_epoch` fields specific to that
architecture's incremental-download bookkeeping and have no broadcasting
equivalent. This also means matching PAwR-ncs's reliability model: no
app-level CRC (BLE's own link-layer CRC is the only integrity check on
either side) and no per-packet timestamp (PDR is computed hub-side from
sequence-number gaps only, exactly like the PAwR distance sweeps in
`THESIS_TECHNICAL_REPORT.md` §3). `hub_main.cpp`'s per-node latency-
correction tracking (which depended on the now-removed timestamp) was
removed along with it. Both environments confirmed rebuilding clean after
this change.

## What's here

Two PlatformIO environments (both `seeed-xiao-afruitnrf52-nrf52840-plus`,
same board family as `central`/`peripheral`'s XIAO nRF52840, matching the
thesis's same-hardware-for-a-fair-comparison approach — see
`THESIS_TECHNICAL_REPORT.md` §6.3):

- **`env:xiao_nrf52840`** (`src/main.cpp` + `lib/`) — the wearable node:
  reads MAX30205 (skin temperature) and SHT4x (humidity) sensors on a
  FreeRTOS timer, broadcasts each reading as a non-connectable/non-scannable
  BLE advertising packet (manufacturer-specific AD type, company ID 0xFFFF,
  8-byte payload identical to PAwR-ncs's own `struct sensor_payload` — see
  `lib/config/DataTypes.h`). No connection, no scheduling coordination with
  any other node — every node advertises independently at its own
  `ADV_INTERVAL_MS`.
- **`env:hub`** (`src/hub_main.cpp`) — passive scanner: validates the
  company ID, tracks per-node sequence numbers (gap/duplicate/out-of-order
  classification, bounded to `MAX_NODES = 32` nodes), and prints one CSV
  row per received packet to the serial console:
  `hub_rx_millis,nodeID,seq,rssi,temp_cdeg,humidity_pct10,flags,dup,gap_count`
  (`temp_cdeg`/`humidity_pct10` are the raw fixed-point wire values, same
  units/format as central's own `EVT DOWNLOAD_DATA ... TEMP %d HUM %u ...`
  console output, for directly comparable captured data) — capture this
  directly (e.g. redirect a serial monitor to a file) for PDR analysis,
  same methodology as the PAwR distance-vs-PDR sweeps in
  `THESIS_TECHNICAL_REPORT.md` §3.1 (received / (seq_max - seq_min + 1)
  from sequence-counter gaps).

`src/i2cscanner.cpp` is a standalone I2C bus-scan utility, excluded from
both environments' `build_src_filter` — a manual bring-up/debug tool, not
part of either firmware.

## Building

This project uses PlatformIO directly, not `west`. If you have the
PlatformIO IDE/VS Code extension installed, open this folder and use its
build/upload buttons per environment. From the CLI (PlatformIO Core):

```
pio run -e xiao_nrf52840   # build the node/broadcaster firmware
pio run -e hub              # build the hub/scanner firmware
pio run -e xiao_nrf52840 -t upload   # build + flash a connected node
pio run -e hub -t upload             # build + flash a connected hub
```

Both environments were confirmed building clean (2026-08-20): `xiao_nrf52840`
14.6% flash / 6.0% RAM, `hub` 14.2% flash / 6.0% RAM.

**Before flashing a node**, set its identity in `lib/config/Config.h`:

```c
#define NODE_ID  32   // change this before uploading to each node
```

(Same pattern as `peripheral/`'s `CONFIG_APP_NODE_ID`, just a `#define`
instead of a Kconfig option — no fixed slot table or central pairing needed
here, since Broadcasting has no scheduling/onboarding step at all.)

## Key parameters (`lib/config/Config.h`)

```c
#define SENSOR_READ_INTERVAL_MS  1000   // how often a new broadcast begins
#define ADV_INTERVAL_MS           100   // BLE advertising repeat rate
```

`ADV_INTERVAL_MS`'s own comment flags that its "< 3% channel occupation for
17 nodes" justification predates the current 18-byte payload and hasn't
been re-derived — worth revisiting analytically or empirically if the
thesis pushes node count materially past 17.
