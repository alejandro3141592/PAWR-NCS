# PAwR sensor dashboard (GUI)

Desktop app (PyQt5) that subscribes to the MQTT broker `gateway_9151`
publishes to, shows one row per node in a live table, plots a history chart,
and presents an interactive 17-body-part human silhouette map with custom sensor node
assignment and live thermal color indicators.

```
gateway_9151 --MQTT/TLS--> HiveMQ Cloud --MQTT/TLS--> this GUI --> SQLite (gui/sensor_data.db)
```

Features 2 central tabs:
- **📊 Table & History**: Live node table & history chart.
- **👤 Body Silhouette**: 17 anatomical body part zones with interactive node mapping,
  thermal color indicators (blue -> cyan -> green -> yellow -> red), and thermoregulation summary stats.

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

## Running Options

### Option 1: Desktop Application (PyQt5)
```sh
python sensor_gui.py
```

### Option 2: Web Dashboard (Streamlit)
```sh
streamlit run streamlit_app.py
```
- Open in any web browser on your PC, laptop, or tablet on your local network (e.g., `http://localhost:8501`).

---

### Features in Both Applications
- **Table & History**: One row per node ID, live temperature/humidity updates, dual-axis history plots backed by SQLite.
- **19 Body Parts Silhouette**: Interactive human body map with Front and Back vector/Plotly views, 19 body part badges (including Neck on Back View, hands, and split feet) displaying assigned Node IDs and real-time thermal color indicators.
- **Node Assignment & Persistence**: Dropdown assignment for nodes 1–19 auto-saved to `body_mapping.json`.
- **Flags column**: `OK`, or `TEMP_FAIL`/`HUM_FAIL` (see
  `SENSOR_PAYLOAD_FLAG_TEMP_INVALID`/`_HUMIDITY_INVALID` in
  `../common/pawr_protocol.h`) if the peripheral itself flagged that
  reading as invalid.
- **History chart**: dual-axis time series (temperature, humidity) for the
  selected node. Time range: 1 h / 6 h / 24 h / 7 d, backed by the SQLite
  log.
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
