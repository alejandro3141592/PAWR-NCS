# PAwR sensor dashboard (GUI)

Desktop app (PyQt5) that subscribes to the MQTT broker `gateway_9151`
publishes to, shows one row per node (latest temperature, humidity,
sequence number, flags, last-seen time) in a live table, logs every reading
to a local SQLite database, and plots a history chart for whichever node is
selected.

```
gateway_9151 --MQTT/TLS--> HiveMQ Cloud --MQTT/TLS--> this GUI --> SQLite (gui/sensor_data.db)
```

Adapted from a prior, richer version of this GUI (body-silhouette mapping,
heart rate, SpO2, multi-channel-per-node) built for a different sensor set.
`gateway_9151` currently only publishes per-node temperature and humidity
(see `../gateway_9151/src/mqtt/mqtt_publisher.c`), so this version is a
plain per-node table instead of the body silhouette -- no HR/SpO2/channel
concepts exist here since nothing produces that data.

## Setup

```sh
pip install -r requirements.txt
```

Copy `config.example.json` to `config.json` (gitignored -- never commit
real credentials) and fill in the real broker password:

```sh
cp config.example.json config.json
# edit config.json, set "password" to the real HiveMQ Cloud password
```

## Running

```sh
python sensor_gui.py
```

- **Table**: one row per node ID seen since launch, sorted by node ID.
  Click a row to select it and show its history chart.
- **Flags column**: `OK`, or `TEMP_FAIL`/`HUM_FAIL` (see
  `SENSOR_PAYLOAD_FLAG_TEMP_INVALID`/`_HUMIDITY_INVALID` in
  `../common/pawr_protocol.h`) if the peripheral itself flagged that
  reading as invalid.
- **History chart**: dual-axis time series (temperature, humidity) for the
  selected node. Time range: 1 h / 6 h / 24 h / 7 d, backed by the SQLite
  log (not just what's arrived since the GUI was opened).
- **Export CSV...**: dumps the entire `readings` table to a CSV file.

## Database

`sensor_data.db` (SQLite, WAL mode) is created automatically in this folder
on first run. Every MQTT message that completes a temperature-or-humidity
update triggers one row insert (both fields may not always be populated
together, e.g. if only one topic has arrived recently -- rows can have a
`NULL` in either `temperature` or `humidity`, not both, since at least one
just arrived to trigger the insert).

```sql
CREATE TABLE readings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          TEXT    NOT NULL,   -- "YYYY-MM-DD HH:MM:SS.mmm"
    node_id     INTEGER NOT NULL,
    seq         INTEGER,            -- peripheral-local rolling counter
    flags       INTEGER,
    temperature REAL,
    humidity    REAL
);
```

## Known gotchas already resolved

- **paho-mqtt v2 API.** The installed `paho-mqtt` here is 2.x, which
  deprecated the old implicit `mqtt.Client()` constructor and changed the
  `on_connect` callback signature (adds a `properties` 5th argument,
  `reason_code` replaces the old integer `rc`). This code uses
  `mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)`
  explicitly and the updated 5-arg `on_connect` signature -- confirmed
  against the actually-installed version, not assumed from older
  paho-mqtt-v1-style reference code.
- **`seq`/`flags` weren't being parsed from the MQTT payload** in an early
  version of this file (only `nodeId`/`value` were extracted) -- caught via
  a live end-to-end test showing `seq` always `None` in the DB despite the
  gateway's JSON payload including it. Fixed by parsing both fields in
  `MQTTWorker.run()`'s `on_message` and threading them through the
  `received` Qt signal.

## Not yet done

- No retry/backoff tuning on MQTT disconnect beyond paho-mqtt's own
  `reconnect_on_failure` default.
- No visual indication in the table when a node hasn't been heard from in a
  while (e.g. greying out a stale row) -- `last_seen` is tracked but not
  yet used for that.
- Not soak-tested over long (multi-hour) runs.
