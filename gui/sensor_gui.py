"""
sensor_gui.py -- PyQt5 real-time skin temperature/humidity dashboard.

Connects to the MQTT broker, shows one row per node (nodeId -> latest temp,
humidity, sequence number, flags, last-seen time), logs every reading to a
local SQLite database, and plots a history chart for whichever node is
selected in the table.

Adapted from a prior, richer version of this GUI (body-silhouette mapping,
heart rate, SpO2, multi-channel-per-node) that doesn't match this project's
current sensor set -- gateway_9151 only publishes per-node temperature and
humidity (see ../gateway_9151/src/mqtt/mqtt_publisher.c), so this version is
a plain per-node table instead.

Requirements:
    pip install -r requirements.txt

Usage:
    1. Copy config.example.json to config.json and fill in the real broker
       password (config.json is gitignored -- never commit real credentials).
    2. python sensor_gui.py
"""

import csv
import json
import re
import ssl
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Optional

from PyQt5.QtCore import Qt, QThread, pyqtSignal
from PyQt5.QtGui import QColor, QFont, QPalette
from PyQt5.QtWidgets import (
    QApplication, QFileDialog, QHBoxLayout, QHeaderView, QLabel,
    QMainWindow, QMessageBox, QPushButton, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget,
)

import paho.mqtt.client as mqtt
import sensor_db

try:
    from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg
    from matplotlib.figure import Figure
    import matplotlib.dates as mdates
    _MPL = True
except ImportError:
    _MPL = False

CONFIG_PATH = Path(__file__).resolve().parent / "config.json"
CONFIG_EXAMPLE_PATH = Path(__file__).resolve().parent / "config.example.json"

TOPICS = ["sensors/temperature", "sensors/humidity"]
_TOPIC_FIELD = {
    "sensors/temperature": "temperature",
    "sensors/humidity": "humidity",
}

TEMP_MIN = 20.0
TEMP_MAX = 42.0

_NAN_RE = re.compile(r':\s*-?(?:nan|inf)\b', re.IGNORECASE)

RANGES = [("1 h", 1), ("6 h", 6), ("24 h", 24), ("7 d", 168)]

FLAG_TEMP_INVALID = 0x01
FLAG_HUMIDITY_INVALID = 0x02


def load_config() -> dict:
    if not CONFIG_PATH.exists():
        raise SystemExit(
            f"Missing {CONFIG_PATH.name}. Copy {CONFIG_EXAMPLE_PATH.name} to "
            f"{CONFIG_PATH.name} and fill in the real broker credentials."
        )
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


@dataclass
class Reading:
    node_id: int
    seq: Optional[int] = None
    flags: int = 0
    temperature: Optional[float] = None
    humidity: Optional[float] = None
    last_seen: Optional[datetime] = None


def temp_color(temp: Optional[float]) -> QColor:
    """Blue (cold) -> cyan -> green -> yellow -> red (hot)."""
    if temp is None:
        return QColor(80, 80, 90)
    t = max(0.0, min(1.0, (temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)))
    if t < 0.25:
        return QColor(0, int(t * 4 * 255), 255)
    if t < 0.5:
        return QColor(0, 255, int((1 - (t - .25) * 4) * 255))
    if t < 0.75:
        return QColor(int((t - .5) * 4 * 255), 255, 0)
    return QColor(255, int((1 - (t - .75) * 4) * 255), 0)


class MQTTWorker(QThread):
    received = pyqtSignal(int, str, float, object, object)
    status = pyqtSignal(str)

    def __init__(self, config: dict, parent=None):
        super().__init__(parent)
        self._config = config

    def run(self):
        cfg = self._config
        # callback_api_version explicit: paho-mqtt v2's VERSION1 (the old,
        # implicit default) is deprecated -- confirmed by testing against
        # the actually-installed paho-mqtt 2.1.0, not assumed from the
        # reference project's (older paho-mqtt v1-style) code.
        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        if cfg.get("username"):
            client.username_pw_set(cfg["username"], cfg.get("password", ""))
        if cfg.get("use_tls", True):
            client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)

        def on_connect(c, userdata, flags, reason_code, properties):
            if reason_code == 0:
                for t in TOPICS:
                    c.subscribe(t, qos=1)
                self.status.emit("Connected")
            else:
                self.status.emit(f"Connection error ({reason_code})")

        def on_message(c, u, msg):
            try:
                payload = _NAN_RE.sub(': null', msg.payload.decode())
                d = json.loads(payload)
            except Exception:
                return
            if d.get("value") is None:
                return
            field = _TOPIC_FIELD.get(msg.topic)
            if field is None:
                return
            try:
                node_id = int(d.get("nodeId"))
                val = float(d.get("value"))
            except (TypeError, ValueError):
                return
            seq = d.get("seq")
            flags = d.get("flags")
            try:
                seq = int(seq) if seq is not None else None
            except (TypeError, ValueError):
                seq = None
            try:
                flags = int(flags) if flags is not None else None
            except (TypeError, ValueError):
                flags = None
            self.received.emit(node_id, field, val, seq, flags)

        client.on_connect = on_connect
        client.on_message = on_message
        self._client = client
        try:
            client.connect(cfg["broker"], cfg.get("port", 8883))
            self.status.emit("Connecting...")
            client.loop_forever()
        except Exception as e:
            self.status.emit(f"Failed: {e}")

    def stop(self):
        if hasattr(self, "_client"):
            self._client.disconnect()


_RANGE_BTN_STYLE = (
    "QPushButton{background:#3c3c3c;color:#ccc;border:1px solid #555;"
    "padding:2px 8px;border-radius:3px;font-size:10px;}"
    "QPushButton:checked{background:#0e639c;color:white;border-color:#1177bb;}"
)


class HistoryChart(QWidget):
    def __init__(self, db_conn, parent=None):
        super().__init__(parent)
        self._db = db_conn
        self._node_id: Optional[int] = None
        self._hours = 1.0
        self._range_btns: Dict[float, QPushButton] = {}

        lay = QVBoxLayout(self)
        lay.setContentsMargins(8, 6, 8, 6)
        lay.setSpacing(6)

        bar = QWidget()
        blay = QHBoxLayout(bar)
        blay.setContentsMargins(0, 0, 0, 0)
        blay.setSpacing(4)

        title = QLabel("History")
        title.setFont(QFont("Arial", 10, QFont.Bold))
        title.setStyleSheet("color:#bbb;")
        blay.addWidget(title)
        blay.addStretch()

        for label, hours in RANGES:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setFixedWidth(44)
            btn.setStyleSheet(_RANGE_BTN_STYLE)
            btn.clicked.connect(lambda _, h=hours: self._set_range(h))
            self._range_btns[hours] = btn
            blay.addWidget(btn)
        self._range_btns[1].setChecked(True)

        lay.addWidget(bar)

        self._placeholder = QLabel("Select a node to view history")
        self._placeholder.setAlignment(Qt.AlignCenter)
        self._placeholder.setStyleSheet("color:#444;font-size:11px;")
        lay.addWidget(self._placeholder, stretch=1)

        if _MPL:
            self._fig = Figure(tight_layout=True)
            self._fig.patch.set_facecolor("#1e1e1e")
            self._canvas = FigureCanvasQTAgg(self._fig)
            self._canvas.setVisible(False)
            lay.addWidget(self._canvas, stretch=1)
        else:
            no_mpl = QLabel("Install matplotlib to enable charts:\npip install matplotlib")
            no_mpl.setAlignment(Qt.AlignCenter)
            no_mpl.setStyleSheet("color:#666;font-size:10px;")
            lay.addWidget(no_mpl, stretch=1)

    def set_node(self, node_id: int):
        self._node_id = node_id
        self._redraw()

    def clear(self):
        self._node_id = None
        if _MPL:
            self._canvas.setVisible(False)
        self._placeholder.setText("Select a node to view history")
        self._placeholder.setVisible(True)

    def refresh_if_active(self, node_id: int):
        if node_id == self._node_id:
            self._redraw()

    def _set_range(self, hours: float):
        for h, btn in self._range_btns.items():
            btn.setChecked(h == hours)
        self._hours = hours
        self._redraw()

    def _redraw(self):
        if not _MPL or self._node_id is None:
            return

        rows = sensor_db.query_history(self._db, self._node_id, self._hours)

        if not rows:
            self._placeholder.setText(
                f"No data for node {self._node_id} in the last "
                + (f"{self._hours:.0f} h" if self._hours < 48 else f"{self._hours/24:.0f} d")
            )
            self._canvas.setVisible(False)
            self._placeholder.setVisible(True)
            return

        self._placeholder.setVisible(False)
        self._canvas.setVisible(True)

        times = [datetime.fromisoformat(r[0]) for r in rows]
        temps = [r[1] for r in rows]
        hums = [r[2] for r in rows]

        self._fig.clear()
        ax1 = self._fig.add_subplot(111)
        ax1.set_facecolor("#252526")

        def filtered(ts, vs):
            pairs = [(t, v) for t, v in zip(ts, vs) if v is not None]
            return zip(*pairs) if pairs else ([], [])

        all_lines = []

        has_temp = any(v is not None for v in temps)
        if has_temp:
            tx, tv = filtered(times, temps)
            all_lines += ax1.plot(tx, tv, color="#ff6b6b", lw=1.6, label="Temp (C)")
            ax1.set_ylabel("Temperature (C)", color="#ff6b6b", fontsize=9)
            ax1.tick_params(axis="y", labelcolor="#ff6b6b", labelsize=8)

        ax2 = ax1.twinx()
        has_hum = any(v is not None for v in hums)
        if has_hum:
            hx, hv = filtered(times, hums)
            all_lines += ax2.plot(hx, hv, color="#6bb5ff", lw=1.6, linestyle="--", label="Humidity (%)")
            ax2.set_ylabel("Humidity (%)", color="#6bb5ff", fontsize=9)
            ax2.tick_params(axis="y", labelcolor="#6bb5ff", labelsize=8)

        if self._hours <= 6:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        elif self._hours <= 48:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%d %H:%M"))
        else:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%d/%m"))
        self._fig.autofmt_xdate(rotation=30)
        ax1.tick_params(axis="x", labelsize=8, colors="#aaa")

        if all_lines:
            ax1.legend(all_lines, [l.get_label() for l in all_lines],
                       loc="upper left", fontsize=8,
                       facecolor="#333", edgecolor="#555", labelcolor="white")

        ax1.grid(True, color="#333", linewidth=0.5)
        for spine in list(ax1.spines.values()) + list(ax2.spines.values()):
            spine.set_color("#444")

        self._canvas.draw_idle()


_TABLE_COLUMNS = ["Node", "Temp (C)", "Humidity (%)", "Seq", "Flags", "Last seen"]


class MainWindow(QMainWindow):
    def __init__(self, config: dict):
        super().__init__()
        self._config = config
        self.setWindowTitle("PAwR Skin Sensor Dashboard")
        self.setMinimumSize(820, 520)
        self.resize(1100, 680)

        self._nodes: Dict[int, Reading] = {}
        self._row_for_node: Dict[int, int] = {}
        self._selected_node: Optional[int] = None

        self._db = sensor_db.open_db()
        self._build_ui()
        self._start_mqtt()

    def _build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        vlay = QVBoxLayout(root)
        vlay.setContentsMargins(0, 0, 0, 0)
        vlay.setSpacing(0)

        bar = QWidget()
        bar.setStyleSheet("background:#252526;")
        bar.setFixedHeight(44)
        blay = QHBoxLayout(bar)
        blay.setContentsMargins(10, 0, 10, 0)

        title = QLabel("PAwR Skin Sensor Dashboard")
        title.setFont(QFont("Arial", 13, QFont.Bold))
        title.setStyleSheet("color:white;")
        blay.addWidget(title)
        blay.addStretch()

        exp_btn = QPushButton("Export CSV...")
        exp_btn.setStyleSheet(
            "background:#3c3c3c;color:white;border:1px solid #555;"
            "padding:4px 10px;border-radius:3px;"
        )
        exp_btn.clicked.connect(self._export_csv)
        blay.addWidget(exp_btn)
        vlay.addWidget(bar)

        content = QWidget()
        content.setStyleSheet("background:#1e1e1e;")
        clay = QHBoxLayout(content)
        clay.setContentsMargins(10, 10, 10, 10)
        clay.setSpacing(10)

        self._table = QTableWidget(0, len(_TABLE_COLUMNS))
        self._table.setHorizontalHeaderLabels(_TABLE_COLUMNS)
        self._table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self._table.verticalHeader().setVisible(False)
        self._table.setSelectionBehavior(QTableWidget.SelectRows)
        self._table.setSelectionMode(QTableWidget.SingleSelection)
        self._table.setEditTriggers(QTableWidget.NoEditTriggers)
        self._table.itemSelectionChanged.connect(self._on_row_selected)
        self._table.setStyleSheet(
            "QTableWidget{background:#252526;color:#ddd;border-radius:8px;"
            "gridline-color:#3c3c3c;}"
            "QHeaderView::section{background:#2d2d2d;color:#bbb;"
            "border:none;padding:4px;}"
        )
        clay.addWidget(self._table, stretch=2)

        self._chart = HistoryChart(self._db)
        self._chart.setStyleSheet("background:#252526;border-radius:8px;")
        clay.addWidget(self._chart, stretch=3)

        vlay.addWidget(content, stretch=1)

        sbar = QWidget()
        sbar.setFixedHeight(28)
        sbar.setStyleSheet("background:#1a1a1a;border-top:1px solid #333;")
        slay = QHBoxLayout(sbar)
        slay.setContentsMargins(10, 0, 10, 0)
        slay.setSpacing(24)
        self._conn_lbl = QLabel("Connecting...")
        self._mean_t_lbl = QLabel("Mean Temp: -")
        self._mean_h_lbl = QLabel("Mean Hum: -")
        self._active_lbl = QLabel("Active nodes: 0")
        self._db_lbl = QLabel(f"DB: {sensor_db.DB_PATH.name}")
        for lbl in [self._conn_lbl, self._mean_t_lbl, self._mean_h_lbl,
                    self._active_lbl, self._db_lbl]:
            lbl.setStyleSheet("color:#777;font-size:10px;")
            slay.addWidget(lbl)
        slay.addStretch()
        vlay.addWidget(sbar)

    def _start_mqtt(self):
        self._mqtt = MQTTWorker(self._config)
        self._mqtt.received.connect(self._on_received)
        self._mqtt.status.connect(self._conn_lbl.setText)
        self._mqtt.start()

    def _on_received(self, node_id: int, field: str, val: float, seq, flags):
        if node_id not in self._nodes:
            self._nodes[node_id] = Reading(node_id=node_id)
        reading = self._nodes[node_id]
        setattr(reading, field, val)
        if seq is not None:
            reading.seq = seq
        if flags is not None:
            reading.flags = flags
        reading.last_seen = datetime.now()

        if field in ("temperature", "humidity"):
            self._maybe_log(reading)

        self._refresh_table()
        self._update_stats()
        self._chart.refresh_if_active(node_id)

    def _maybe_log(self, reading: Reading):
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        sensor_db.insert_reading(
            self._db, ts, reading.node_id, reading.seq, reading.flags,
            reading.temperature, reading.humidity,
        )

    def _refresh_table(self):
        nodes = sorted(self._nodes.keys())
        self._table.setRowCount(len(nodes))
        self._row_for_node.clear()

        for row, node_id in enumerate(nodes):
            self._row_for_node[node_id] = row
            r = self._nodes[node_id]

            node_item = QTableWidgetItem(str(node_id))
            temp_item = QTableWidgetItem(f"{r.temperature:.2f}" if r.temperature is not None else "-")
            hum_item = QTableWidgetItem(f"{r.humidity:.1f}" if r.humidity is not None else "-")
            seq_item = QTableWidgetItem(str(r.seq) if r.seq is not None else "-")
            flags_item = QTableWidgetItem(self._format_flags(r.flags))
            seen_item = QTableWidgetItem(r.last_seen.strftime("%H:%M:%S") if r.last_seen else "-")

            temp_item.setForeground(temp_color(r.temperature))

            for item in (node_item, temp_item, hum_item, seq_item, flags_item, seen_item):
                item.setTextAlignment(Qt.AlignCenter)

            for col, item in enumerate(
                (node_item, temp_item, hum_item, seq_item, flags_item, seen_item)
            ):
                self._table.setItem(row, col, item)

        if self._selected_node is not None and self._selected_node in self._row_for_node:
            self._table.selectRow(self._row_for_node[self._selected_node])

    @staticmethod
    def _format_flags(flags: int) -> str:
        if not flags:
            return "OK"
        parts = []
        if flags & FLAG_TEMP_INVALID:
            parts.append("TEMP_FAIL")
        if flags & FLAG_HUMIDITY_INVALID:
            parts.append("HUM_FAIL")
        return ",".join(parts) if parts else f"0x{flags:02x}"

    def _on_row_selected(self):
        items = self._table.selectedItems()
        if not items:
            self._selected_node = None
            self._chart.clear()
            return
        row = items[0].row()
        node_id = int(self._table.item(row, 0).text())
        self._selected_node = node_id
        self._chart.set_node(node_id)

    def _update_stats(self):
        temps = [r.temperature for r in self._nodes.values() if r.temperature is not None]
        hums = [r.humidity for r in self._nodes.values() if r.humidity is not None]
        self._mean_t_lbl.setText(
            f"Mean Temp: {sum(temps)/len(temps):.1f} C" if temps else "Mean Temp: -"
        )
        self._mean_h_lbl.setText(
            f"Mean Hum: {sum(hums)/len(hums):.1f} %" if hums else "Mean Hum: -"
        )
        self._active_lbl.setText(f"Active nodes: {len(self._nodes)}")

    def _export_csv(self):
        path, _ = QFileDialog.getSaveFileName(
            self, "Export CSV", "sensor_export.csv", "CSV files (*.csv)"
        )
        if not path:
            return
        rows = sensor_db.query_all_for_export(self._db)
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["datetime", "nodeId", "seq", "flags", "temperature", "humidity"])
            w.writerows(rows)
        QMessageBox.information(self, "Export complete", f"Saved {len(rows)} rows to:\n{path}")

    def closeEvent(self, event):
        self._mqtt.stop()
        self._mqtt.wait(2000)
        self._db.close()
        super().closeEvent(event)


def main():
    config = load_config()

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    pal = QPalette()
    pal.setColor(QPalette.Window, QColor(30, 30, 30))
    pal.setColor(QPalette.WindowText, QColor(220, 220, 220))
    pal.setColor(QPalette.Base, QColor(40, 40, 40))
    pal.setColor(QPalette.AlternateBase, QColor(50, 50, 50))
    pal.setColor(QPalette.Text, QColor(220, 220, 220))
    pal.setColor(QPalette.Button, QColor(60, 60, 60))
    pal.setColor(QPalette.ButtonText, QColor(220, 220, 220))
    pal.setColor(QPalette.Highlight, QColor(38, 120, 215))
    pal.setColor(QPalette.HighlightedText, Qt.white)
    app.setPalette(pal)

    win = MainWindow(config)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
