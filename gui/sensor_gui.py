"""
sensor_gui.py -- PyQt5 real-time skin temperature/humidity dashboard.

Premium, state-of-the-art medical thermography interface with glassmorphic panels,
neon vector body outlines, FLIR thermal radial heatmaps, node health status tracking,
multi-person 4-subject monitoring (2x2 grid view), and PNG map image export.

Requirements:
    pip install -r requirements.txt

Usage:
    1. Copy config.example.json to config.json and fill in real broker credentials.
    2. python sensor_gui.py
"""

import csv
import json
import math
import ssl
import struct
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from PyQt5.QtCore import QPointF, QRectF, Qt, QThread, QTimer, pyqtSignal
from PyQt5.QtGui import QBrush, QColor, QFont, QLinearGradient, QPainter, QPainterPath, QPalette, QPen, QPixmap, QRadialGradient
from PyQt5.QtWidgets import (
    QApplication, QComboBox, QFileDialog, QFrame, QGridLayout, QGroupBox, QHBoxLayout,
    QHeaderView, QLabel, QLineEdit, QMainWindow, QMessageBox, QPushButton, QScrollArea,
    QStackedWidget, QTabWidget, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
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
BODY_MAPPING_PATH = Path(__file__).resolve().parent / "body_mapping.json"

TOPICS = ["sensors/data"]

# Wire format for sensors/data: raw bytes of the firmware's struct
# sensor_payload (see ../common/pawr_protocol.h), little-endian --
# node_id(u8), flags(u8), seq(u16), temp_cdeg(i16), humidity_pct10(u16).
# Replaces the prior per-field JSON publishes (sensors/temperature,
# sensors/humidity) with one compact binary message per node per interval,
# see NOTES.md 2026-08-04 for the cellular-data-usage motivation.
_SENSOR_PAYLOAD_STRUCT = struct.Struct("<BBHhH")

TEMP_MIN = 20.0
TEMP_MAX = 42.0
NUM_PERSONS = 4

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

    def health(self) -> str:
        if not self.last_seen:
            return "OFFLINE"
        elapsed = (datetime.now() - self.last_seen).total_seconds()
        if elapsed < 15.0:
            return "ACTIVE"
        elif elapsed < 60.0:
            return "IDLE"
        else:
            return "OFFLINE"


@dataclass
class BodyPart:
    id: int
    name: str
    short_name: str
    view: str       # "front" or "back"
    rel_x: float    # 0.0 to 1.0 within silhouette view
    rel_y: float    # 0.0 to 1.0 within silhouette view


BODY_PARTS: List[BodyPart] = [
    BodyPart(1,  "Head / Forehead",      "Head",       "front", 0.50, 0.07),
    BodyPart(2,  "Neck",                 "Neck",       "back",  0.50, 0.15),
    BodyPart(3,  "Upper Chest",          "Chest",      "front", 0.50, 0.25),
    BodyPart(4,  "Abdomen",              "Abdomen",    "front", 0.50, 0.38),
    BodyPart(5,  "Upper Back",           "Upper Back", "back",  0.50, 0.27),
    BodyPart(6,  "Lower Back",           "Lower Back", "back",  0.50, 0.40),
    BodyPart(7,  "Left Shoulder",        "L.Shoulder", "front", 0.22, 0.21),
    BodyPart(8,  "Right Shoulder",       "R.Shoulder", "front", 0.78, 0.21),
    BodyPart(9,  "Left Upper Arm",       "L.Bicep",    "front", 0.14, 0.32),
    BodyPart(10, "Right Upper Arm",      "R.Bicep",    "front", 0.86, 0.32),
    BodyPart(11, "Left Forearm",         "L.Forearm",  "front", 0.10, 0.44),
    BodyPart(12, "Right Forearm",        "R.Forearm",  "front", 0.90, 0.44),
    BodyPart(13, "Left Hand",            "L.Hand",     "front", 0.06, 0.54),
    BodyPart(14, "Right Hand",           "R.Hand",     "front", 0.94, 0.54),
    BodyPart(15, "Left Thigh",           "L.Thigh",    "front", 0.36, 0.57),
    BodyPart(16, "Right Thigh",          "R.Thigh",    "front", 0.64, 0.57),
    BodyPart(17, "Shin & Calf",          "Shin/Calf",  "front", 0.50, 0.76),
    BodyPart(18, "Left Foot",            "L.Foot",     "front", 0.36, 0.92),
    BodyPart(19, "Right Foot",           "R.Foot",     "front", 0.64, 0.92),
]


@dataclass
class PersonConfig:
    person_id: int
    name: str
    mapping: Dict[int, Optional[int]]


def load_multi_person_config() -> List[PersonConfig]:
    default_persons = []
    for pid in range(1, NUM_PERSONS + 1):
        mapping = {bp.id: (bp.id if pid == 1 else None) for bp in BODY_PARTS}
        default_persons.append(PersonConfig(person_id=pid, name=f"Subject {pid:02d}", mapping=mapping))

    if not BODY_MAPPING_PATH.exists():
        return default_persons

    try:
        with open(BODY_MAPPING_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)

        if "person_1" not in data and "1" in data:
            result = default_persons
            m1 = {bp.id: int(data[str(bp.id)]) if data.get(str(bp.id)) is not None else None for bp in BODY_PARTS}
            result[0].mapping = m1
            return result

        result = []
        for pid in range(1, NUM_PERSONS + 1):
            key = f"person_{pid}"
            p_data = data.get(key, {})
            name = p_data.get("name", f"Subject {pid:02d}")
            raw_m = p_data.get("mapping", {})
            mapping = {}
            for bp in BODY_PARTS:
                val = raw_m.get(str(bp.id))
                mapping[bp.id] = int(val) if val is not None else None
            result.append(PersonConfig(person_id=pid, name=name, mapping=mapping))
        return result
    except Exception as e:
        print(f"Error loading multi-person config: {e}")
        return default_persons


def save_multi_person_config(persons: List[PersonConfig]) -> None:
    try:
        data = {}
        for p in persons:
            key = f"person_{p.person_id}"
            data[key] = {
                "name": p.name,
                "mapping": {str(k): v for k, v in p.mapping.items()}
            }
        with open(BODY_MAPPING_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except Exception as e:
        print(f"Failed to save multi-person mapping: {e}")


def temp_color(temp: Optional[float]) -> QColor:
    """Rich FLIR thermal gradient: Blue (cold) -> Cyan -> Green -> Yellow -> Red -> Magenta (hot)."""
    if temp is None:
        return QColor(42, 45, 58)
    t = max(0.0, min(1.0, (temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN)))
    if t < 0.2:
        return QColor(0, int(t * 5 * 200), 255)
    if t < 0.4:
        return QColor(0, 240, int((1 - (t - .2) * 5) * 255))
    if t < 0.65:
        return QColor(int((t - .4) * 4 * 255), 240, 0)
    if t < 0.85:
        return QColor(255, int((1 - (t - .65) * 5) * 220), 0)
    return QColor(255, 0, int((t - .85) * 6.6 * 200))


class MQTTWorker(QThread):
    received = pyqtSignal(int, str, float, object, object)
    status = pyqtSignal(str)

    def __init__(self, config: dict, parent=None):
        super().__init__(parent)
        self._config = config

    def run(self):
        cfg = self._config
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
            if msg.topic != "sensors/data":
                return
            try:
                node_id, flags, seq, temp_cdeg, humidity_pct10 = \
                    _SENSOR_PAYLOAD_STRUCT.unpack(msg.payload)
            except struct.error:
                return

            # Mirrors the firmware's own skip-if-invalid behavior (see
            # gateway_9151/src/mqtt/mqtt_publisher.c / SENSOR_PAYLOAD_FLAG_*
            # in pawr_protocol.h) -- one message carries both fields, so
            # each is only forwarded if its own flag bit isn't set.
            if not (flags & FLAG_TEMP_INVALID):
                self.received.emit(node_id, "temperature", temp_cdeg / 100.0, seq, flags)
            if not (flags & FLAG_HUMIDITY_INVALID):
                self.received.emit(node_id, "humidity", humidity_pct10 / 10.0, seq, flags)

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
    "QPushButton{background:#222533;color:#aaa;border:1px solid #3b4259;"
    "padding:2px 8px;border-radius:4px;font-size:10px;font-weight:bold;}"
    "QPushButton:checked{background:#0e639c;color:#00e5ff;border-color:#00e5ff;}"
    "QPushButton:hover{background:#2d3345;color:white;}"
)


class HistoryChart(QWidget):
    def __init__(self, db_conn, parent=None):
        super().__init__(parent)
        self._db = db_conn
        self._node_id: Optional[int] = None
        self._hours = 1.0
        self._range_btns: Dict[float, QPushButton] = {}

        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 8, 10, 8)
        lay.setSpacing(6)

        bar = QWidget()
        blay = QHBoxLayout(bar)
        blay.setContentsMargins(0, 0, 0, 0)
        blay.setSpacing(4)

        title = QLabel("Telemetry History")
        title.setFont(QFont("Segoe UI", 10, QFont.Bold))
        title.setStyleSheet("color:#38bdf8;")
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

        self._placeholder = QLabel("Select a node from table to plot historical thermals")
        self._placeholder.setAlignment(Qt.AlignCenter)
        self._placeholder.setStyleSheet("color:#64748b;font-size:11px;")
        lay.addWidget(self._placeholder, stretch=1)

        if _MPL:
            self._fig = Figure(tight_layout=True)
            self._fig.patch.set_facecolor("#161822")
            self._canvas = FigureCanvasQTAgg(self._fig)
            self._canvas.setVisible(False)
            lay.addWidget(self._canvas, stretch=1)
        else:
            no_mpl = QLabel("Install matplotlib to enable charts:\npip install matplotlib")
            no_mpl.setAlignment(Qt.AlignCenter)
            no_mpl.setStyleSheet("color:#64748b;font-size:10px;")
            lay.addWidget(no_mpl, stretch=1)

    def set_node(self, node_id: int):
        self._node_id = node_id
        self._redraw()

    def clear(self):
        self._node_id = None
        if _MPL:
            self._canvas.setVisible(False)
        self._placeholder.setText("Select a node from table to plot historical thermals")
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
        ax1.set_facecolor("#1c1f2e")

        def filtered(ts, vs):
            pairs = [(t, v) for t, v in zip(ts, vs) if v is not None]
            return zip(*pairs) if pairs else ([], [])

        all_lines = []

        has_temp = any(v is not None for v in temps)
        if has_temp:
            tx, tv = filtered(times, temps)
            all_lines += ax1.plot(tx, tv, color="#ff4757", lw=1.8, label="Temp (C)")
            ax1.set_ylabel("Temperature (°C)", color="#ff4757", fontsize=9, fontweight="bold")
            ax1.tick_params(axis="y", labelcolor="#ff4757", labelsize=8)

        ax2 = ax1.twinx()
        has_hum = any(v is not None for v in hums)
        if has_hum:
            hx, hv = filtered(times, hums)
            all_lines += ax2.plot(hx, hv, color="#00e5ff", lw=1.8, linestyle="--", label="Humidity (%)")
            ax2.set_ylabel("Humidity (%)", color="#00e5ff", fontsize=9, fontweight="bold")
            ax2.tick_params(axis="y", labelcolor="#00e5ff", labelsize=8)

        if self._hours <= 6:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        elif self._hours <= 48:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%d %H:%M"))
        else:
            ax1.xaxis.set_major_formatter(mdates.DateFormatter("%d/%m"))
        self._fig.autofmt_xdate(rotation=30)
        ax1.tick_params(axis="x", labelsize=8, colors="#94a3b8")

        if all_lines:
            ax1.legend(all_lines, [l.get_label() for l in all_lines],
                       loc="upper left", fontsize=8,
                       facecolor="#222533", edgecolor="#3b4259", labelcolor="white")

        ax1.grid(True, color="#2d3245", linewidth=0.6, linestyle=":")
        for spine in list(ax1.spines.values()) + list(ax2.spines.values()):
            spine.set_color("#3b4259")

        self._canvas.draw_idle()


class BodySilhouetteCanvas(QWidget):
    """Futuristic custom QPainter canvas rendering neon vector silhouettes,
    multi-layer FLIR radial thermal heatmaps, and glassmorphic node badges."""

    part_clicked = pyqtSignal(int)

    def __init__(self, is_compact: bool = False, parent=None):
        super().__init__(parent)
        self.is_compact = is_compact
        self.setMinimumSize(220 if is_compact else 480, 260 if is_compact else 520)
        self._mapping: Dict[int, Optional[int]] = {}
        self._nodes: Dict[int, Reading] = {}
        self._selected_part_id: Optional[int] = None
        self._badge_rects: Dict[int, QRectF] = {}

    def set_mapping(self, mapping: Dict[int, Optional[int]]):
        self._mapping = mapping
        self.update()

    def update_readings(self, nodes: Dict[int, Reading]):
        self._nodes = nodes
        self.update()

    def set_selected_part(self, part_id: Optional[int]):
        self._selected_part_id = part_id
        self.update()

    def mousePressEvent(self, event):
        if self.is_compact:
            return
        pos = event.pos()
        for part_id, rect in self._badge_rects.items():
            if rect.contains(QPointF(pos)):
                self._selected_part_id = part_id
                self.part_clicked.emit(part_id)
                self.update()
                return

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        w, h = self.width(), self.height()
        half_w = w / 2.0

        # Background Fill
        painter.fillRect(self.rect(), QColor(18, 19, 24))

        # Subtle Lab Sci-Fi Grid Lines
        if not self.is_compact:
            painter.setPen(QPen(QColor(0, 180, 216, 18), 1, Qt.DotLine))
            grid_step = 36
            for x_g in range(0, w, grid_step):
                painter.drawLine(x_g, 0, x_g, h)
            for y_g in range(0, h, grid_step):
                painter.drawLine(0, y_g, w, y_g)

        front_rect = QRectF(0, 0, half_w, h)
        back_rect = QRectF(half_w, 0, half_w, h)

        if not self.is_compact:
            painter.setFont(QFont("Segoe UI", 10, QFont.Bold))
            painter.setPen(QPen(QColor(56, 189, 248)))
            painter.drawText(front_rect.adjusted(0, 12, 0, 0), Qt.AlignHCenter | Qt.AlignTop, "ANTERIOR (FRONT)")
            painter.drawText(back_rect.adjusted(0, 12, 0, 0), Qt.AlignHCenter | Qt.AlignTop, "POSTERIOR (BACK)")

            painter.setPen(QPen(QColor(59, 66, 89), 1, Qt.DashLine))
            painter.drawLine(int(half_w), 15, int(half_w), h - 15)

        margin_top = 25 if self.is_compact else 45
        available_h = h - margin_top - (10 if self.is_compact else 20)

        # 1. Multi-layered FLIR Thermal Heatmap Overlays
        self._draw_thermal_heatmap(painter, front_rect, back_rect, margin_top, available_h)

        # 2. Neon Vector Silhouette Contours
        self._draw_silhouette(painter, front_rect, is_front=True)
        self._draw_silhouette(painter, back_rect, is_front=False)

        # 3. 19 Body Part Glass Badges
        self._badge_rects.clear()

        for bp in BODY_PARTS:
            view_rect = front_rect if bp.view == "front" else back_rect
            cx = view_rect.x() + view_rect.width() * bp.rel_x
            cy = view_rect.y() + margin_top + available_h * bp.rel_y

            node_id = self._mapping.get(bp.id)
            reading = self._nodes.get(node_id) if node_id is not None else None
            temp = reading.temperature if reading else None
            health = reading.health() if reading else "OFFLINE"

            bg_color = temp_color(temp) if (node_id is not None and temp is not None) else QColor(32, 35, 46)
            is_selected = (bp.id == self._selected_part_id)

            badge_w = 64.0 if self.is_compact else 102.0
            badge_h = 20.0 if self.is_compact else 32.0
            badge_rect = QRectF(cx - badge_w / 2.0, cy - badge_h / 2.0, badge_w, badge_h)
            self._badge_rects[bp.id] = badge_rect

            # Center Pulsing Dot
            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(bg_color))
            painter.drawEllipse(QPointF(cx, cy), 3.0 if self.is_compact else 4.5, 3.0 if self.is_compact else 4.5)

            # Glassmorphic Pill Badge
            painter.setBrush(QBrush(bg_color))
            if is_selected:
                painter.setPen(QPen(QColor(0, 229, 255), 2.2))
            elif health == "OFFLINE" and node_id is not None:
                painter.setPen(QPen(QColor(239, 68, 68), 1.2, Qt.DashLine))
            elif health == "IDLE":
                painter.setPen(QPen(QColor(245, 158, 11), 1.2))
            else:
                painter.setPen(QPen(QColor(59, 66, 89), 1.0))

            painter.drawRoundedRect(badge_rect, 5.0 if self.is_compact else 7.0, 5.0 if self.is_compact else 7.0)

            # Health Dot Ring
            health_color = QColor(34, 197, 94) if health == "ACTIVE" else (QColor(245, 158, 11) if health == "IDLE" else QColor(239, 68, 68))
            if node_id is not None:
                painter.setPen(Qt.NoPen)
                painter.setBrush(QBrush(health_color))
                painter.drawEllipse(QPointF(badge_rect.right() - 6, badge_rect.top() + 6), 3.0, 3.0)

            # Badge Typography
            painter.setPen(QPen(QColor(255, 255, 255) if bg_color.lightness() < 170 else QColor(10, 10, 10)))
            if self.is_compact:
                painter.setFont(QFont("Segoe UI", 7, QFont.Bold))
                val_str = f"N{node_id:02d}:{temp:.0f}°" if (node_id and temp) else f"{bp.short_name[:3]}"
                painter.drawText(badge_rect, Qt.AlignCenter, val_str)
            else:
                painter.setFont(QFont("Segoe UI", 8, QFont.Bold))
                r_title = QRectF(badge_rect.x() + 2, badge_rect.y() + 2, badge_rect.width() - 4, 14)
                painter.drawText(r_title, Qt.AlignCenter, bp.short_name)

                painter.setFont(QFont("Segoe UI", 8, QFont.Normal))
                r_val = QRectF(badge_rect.x() + 2, badge_rect.y() + 16, badge_rect.width() - 4, 14)
                val_str = "Unassigned" if node_id is None else (f"N{node_id:02d}: -- °C" if temp is None else f"N{node_id:02d}: {temp:.1f} °C")
                painter.drawText(r_val, Qt.AlignCenter, val_str)

    def _draw_thermal_heatmap(self, painter: QPainter, front_rect: QRectF, back_rect: QRectF, margin_top: float, available_h: float):
        painter.save()
        painter.setCompositionMode(QPainter.CompositionMode_Screen)

        for bp in BODY_PARTS:
            node_id = self._mapping.get(bp.id)
            if node_id is None:
                continue
            reading = self._nodes.get(node_id)
            if not reading or reading.temperature is None:
                continue

            view_rect = front_rect if bp.view == "front" else back_rect
            cx = view_rect.x() + view_rect.width() * bp.rel_x
            cy = view_rect.y() + margin_top + available_h * bp.rel_y

            base_col = temp_color(reading.temperature)
            r, g, b = base_col.red(), base_col.green(), base_col.blue()

            glow_radius = 32.0 if self.is_compact else 54.0
            grad = QRadialGradient(QPointF(cx, cy), glow_radius)
            grad.setColorAt(0.0, QColor(r, g, b, 210))
            grad.setColorAt(0.35, QColor(r, g, b, 120))
            grad.setColorAt(0.70, QColor(r, g, b, 45))
            grad.setColorAt(1.0, QColor(r, g, b, 0))

            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(grad))
            painter.drawEllipse(QPointF(cx, cy), glow_radius, glow_radius)

        painter.restore()

    def _draw_silhouette(self, painter: QPainter, rect: QRectF, is_front: bool):
        cx = rect.center().x()
        top = rect.y() + (20 if self.is_compact else 45)
        bh = rect.height() - (30 if self.is_compact else 65)

        # Outer Neon Glow Stroke
        painter.setPen(QPen(QColor(0, 180, 216, 50), 4.0 if not self.is_compact else 2.5))
        painter.setBrush(QBrush(QColor(20, 26, 38, 160)))

        # Head
        h_cx, h_cy = cx, top + bh * 0.07
        h_rx, h_ry = bh * 0.045, bh * 0.06
        painter.drawEllipse(QPointF(h_cx, h_cy), h_rx, h_ry)

        # Body Outline Path
        path = QPainterPath()
        path.moveTo(cx - bh * 0.02, top + bh * 0.13)
        path.lineTo(cx - bh * 0.13, top + bh * 0.18)
        path.lineTo(cx - bh * 0.18, top + bh * 0.32)
        path.lineTo(cx - bh * 0.22, top + bh * 0.46)
        path.lineTo(cx - bh * 0.17, top + bh * 0.46)
        path.lineTo(cx - bh * 0.13, top + bh * 0.33)
        path.lineTo(cx - bh * 0.09, top + bh * 0.38)
        path.lineTo(cx - bh * 0.10, top + bh * 0.50)
        path.lineTo(cx - bh * 0.09, top + bh * 0.72)
        path.lineTo(cx - bh * 0.08, top + bh * 0.90)
        path.lineTo(cx - bh * 0.12, top + bh * 0.93)
        path.lineTo(cx - bh * 0.04, top + bh * 0.93)
        path.lineTo(cx - bh * 0.02, top + bh * 0.52)
        path.lineTo(cx, top + bh * 0.51)
        path.lineTo(cx + bh * 0.02, top + bh * 0.52)
        path.lineTo(cx + bh * 0.04, top + bh * 0.93)
        path.lineTo(cx + bh * 0.12, top + bh * 0.93)
        path.lineTo(cx + bh * 0.08, top + bh * 0.90)
        path.lineTo(cx + bh * 0.09, top + bh * 0.72)
        path.lineTo(cx + bh * 0.10, top + bh * 0.50)
        path.lineTo(cx + bh * 0.09, top + bh * 0.38)
        path.lineTo(cx + bh * 0.13, top + bh * 0.33)
        path.lineTo(cx + bh * 0.17, top + bh * 0.46)
        path.lineTo(cx + bh * 0.22, top + bh * 0.46)
        path.lineTo(cx + bh * 0.18, top + bh * 0.32)
        path.lineTo(cx + bh * 0.13, top + bh * 0.18)
        path.lineTo(cx + bh * 0.02, top + bh * 0.13)
        path.closeSubpath()

        painter.drawPath(path)

        # Sharp Inner Neon Line
        painter.setPen(QPen(QColor(0, 180, 216, 220), 1.5))
        painter.setBrush(Qt.NoBrush)
        painter.drawEllipse(QPointF(h_cx, h_cy), h_rx, h_ry)
        painter.drawPath(path)


class PersonCardWidget(QWidget):
    """Grid item widget showing mini silhouette and thermals summary for one Person."""

    inspect_requested = pyqtSignal(int)

    def __init__(self, person: PersonConfig, parent=None):
        super().__init__(parent)
        self.person = person
        self.setStyleSheet("QWidget{background:#161822;border-radius:10px;border:1px solid #2d3245;}")

        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 10, 10, 10)
        lay.setSpacing(6)

        # Header bar
        hbar = QHBoxLayout()
        self.lbl_title = QLabel(f"👤 {person.name}")
        self.lbl_title.setFont(QFont("Segoe UI", 10, QFont.Bold))
        self.lbl_title.setStyleSheet("color:#f0f2f5;")
        hbar.addWidget(self.lbl_title)
        hbar.addStretch()

        btn_inspect = QPushButton("Inspect / Map 🔍")
        btn_inspect.setStyleSheet(
            "QPushButton{background:#0e639c;color:white;padding:4px 10px;border-radius:4px;font-size:10px;font-weight:bold;border:none;}"
            "QPushButton:hover{background:#00e5ff;color:black;}"
        )
        btn_inspect.clicked.connect(lambda: self.inspect_requested.emit(person.person_id))
        hbar.addWidget(btn_inspect)
        lay.addLayout(hbar)

        # Mini Canvas
        self.canvas = BodySilhouetteCanvas(is_compact=True)
        self.canvas.set_mapping(person.mapping)
        lay.addWidget(self.canvas, stretch=1)

        # Stats bar
        self.lbl_stats = QLabel("Mean: - °C | Active: 0")
        self.lbl_stats.setAlignment(Qt.AlignCenter)
        self.lbl_stats.setStyleSheet("color:#94a3b8;font-size:10px;font-weight:bold;")
        lay.addWidget(self.lbl_stats)

    def update_readings(self, nodes: Dict[int, Reading]):
        self.canvas.update_readings(nodes)
        self.lbl_title.setText(f"👤 {self.person.name}")

        valid_temps = []
        active_cnt = 0
        for bp in BODY_PARTS:
            nid = self.person.mapping.get(bp.id)
            if nid is not None and nid in nodes:
                r = nodes[nid]
                if r.health() == "ACTIVE":
                    active_cnt += 1
                if r.temperature is not None:
                    valid_temps.append(r.temperature)

        if valid_temps:
            avg_t = sum(valid_temps) / len(valid_temps)
            self.lbl_stats.setText(f"Mean: {avg_t:.1f} °C | Active: {active_cnt} nodes")
        else:
            self.lbl_stats.setText(f"Mean: - °C | Active: {active_cnt} nodes")


class BodyMapTab(QWidget):
    """Main tab container holding 2x2 Grid View and Single Person Focus view."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.persons = load_multi_person_config()
        self._nodes: Dict[int, Reading] = {}
        self.current_person_id = 1

        self._combos: Dict[int, QComboBox] = {}
        self._row_widgets: Dict[int, QWidget] = {}

        self._build_ui()

    def _build_ui(self):
        root_lay = QVBoxLayout(self)
        root_lay.setContentsMargins(10, 10, 10, 10)
        root_lay.setSpacing(8)

        # View Mode Header
        mode_bar = QWidget()
        mode_bar.setStyleSheet("background:#161822;border-radius:8px;border:1px solid #2d3245;")
        m_lay = QHBoxLayout(mode_bar)
        m_lay.setContentsMargins(10, 6, 10, 6)
        m_lay.setSpacing(10)

        m_title = QLabel("View Mode:")
        m_title.setFont(QFont("Segoe UI", 10, QFont.Bold))
        m_title.setStyleSheet("color:#38bdf8;")
        m_lay.addWidget(m_title)

        self.combo_mode = QComboBox()
        self.combo_mode.setStyleSheet(
            "QComboBox{background:#222533;color:#fff;border:1px solid #3b4259;padding:4px 10px;border-radius:5px;font-weight:bold;}"
            "QComboBox QAbstractItemView{background:#161822;color:#fff;selection-background-color:#0e639c;}"
        )
        self.combo_mode.addItem("👥 2x2 Grid View (All 4 Persons)", 0)
        for p in self.persons:
            self.combo_mode.addItem(f"👤 {p.name}", p.person_id)

        self.combo_mode.currentIndexChanged.connect(self._on_view_mode_changed)
        m_lay.addWidget(self.combo_mode)
        m_lay.addStretch()

        root_lay.addWidget(mode_bar)

        self.stack = QStackedWidget()

        # PAGE 0: 2x2 GRID VIEW
        grid_page = QWidget()
        g_lay = QGridLayout(grid_page)
        g_lay.setContentsMargins(0, 0, 0, 0)
        g_lay.setSpacing(10)

        self.cards: List[PersonCardWidget] = []
        positions = [(0, 0), (0, 1), (1, 0), (1, 1)]
        for idx, p in enumerate(self.persons):
            card = PersonCardWidget(p)
            card.inspect_requested.connect(self._inspect_person)
            self.cards.append(card)
            r, c = positions[idx]
            g_lay.addWidget(card, r, c)

        self.stack.addWidget(grid_page)

        # PAGE 1: SINGLE PERSON FOCUS VIEW
        focus_page = QWidget()
        f_lay = QHBoxLayout(focus_page)
        f_lay.setContentsMargins(0, 0, 0, 0)
        f_lay.setSpacing(10)

        self.canvas = BodySilhouetteCanvas(is_compact=False)
        self.canvas.part_clicked.connect(self._on_part_clicked_canvas)
        f_lay.addWidget(self.canvas, stretch=3)

        sidebar = QWidget()
        sidebar.setStyleSheet("background:#161822;border-radius:10px;border:1px solid #2d3245;")
        sb_lay = QVBoxLayout(sidebar)
        sb_lay.setContentsMargins(12, 12, 12, 12)
        sb_lay.setSpacing(8)

        pname_lay = QHBoxLayout()
        pname_lbl = QLabel("Subject Name:")
        pname_lbl.setStyleSheet("color:#94a3b8;font-size:11px;font-weight:bold;")
        pname_lay.addWidget(pname_lbl)

        self.input_pname = QLineEdit()
        self.input_pname.setStyleSheet("QLineEdit{background:#222533;color:#fff;border:1px solid #3b4259;padding:4px 8px;border-radius:4px;font-weight:bold;}")
        self.input_pname.textChanged.connect(self._on_person_name_changed)
        pname_lay.addWidget(self.input_pname)
        sb_lay.addLayout(pname_lay)

        act_box = QHBoxLayout()
        btn_def = QPushButton("1-to-1 Default")
        btn_def.setStyleSheet("background:#222533;color:#eee;padding:5px 10px;border-radius:4px;font-size:10px;font-weight:bold;border:1px solid #3b4259;")
        btn_def.clicked.connect(self._reset_default_mapping)
        act_box.addWidget(btn_def)

        btn_clr = QPushButton("Unassign All")
        btn_clr.setStyleSheet("background:#222533;color:#eee;padding:5px 10px;border-radius:4px;font-size:10px;font-weight:bold;border:1px solid #3b4259;")
        btn_clr.clicked.connect(self._clear_all_mapping)
        act_box.addWidget(btn_clr)

        sb_lay.addLayout(act_box)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet("QScrollArea{border:none;background:transparent;}")
        scroll_content = QWidget()
        scroll_lay = QVBoxLayout(scroll_content)
        scroll_lay.setContentsMargins(0, 0, 0, 0)
        scroll_lay.setSpacing(4)

        for bp in BODY_PARTS:
            row = QWidget()
            row.setStyleSheet("QWidget{background:#1c1f2e;border-radius:5px;padding:2px;}")
            rlay = QHBoxLayout(row)
            rlay.setContentsMargins(8, 4, 8, 4)

            lbl = QLabel(f"{bp.id}. {bp.name}")
            lbl.setFont(QFont("Segoe UI", 9))
            lbl.setStyleSheet("color:#cbd5e1;")
            rlay.addWidget(lbl, stretch=1)

            combo = QComboBox()
            combo.setFixedWidth(110)
            combo.setStyleSheet(
                "QComboBox{background:#252836;color:#eee;border:1px solid #3b4259;"
                "padding:2px 4px;border-radius:4px;}"
                "QComboBox QAbstractItemView{background:#161822;color:#eee;selection-background-color:#0e639c;}"
            )
            combo.addItem("Unassigned", None)
            for nid in range(1, 39):
                combo.addItem(f"Node {nid:02d}", nid)

            combo.currentIndexChanged.connect(lambda idx, bpid=bp.id: self._on_combo_changed(bpid, idx))
            self._combos[bp.id] = combo
            self._row_widgets[bp.id] = row
            rlay.addWidget(combo)

            scroll_lay.addWidget(row)

        scroll_content.setLayout(scroll_lay)
        scroll.setWidget(scroll_content)
        sb_lay.addWidget(scroll, stretch=1)

        summary_gb = QGroupBox("Body Thermals Summary")
        summary_gb.setStyleSheet("QGroupBox{color:#38bdf8;font-weight:bold;border:1px solid #3b4259;margin-top:6px;padding-top:10px;}")
        sg_lay = QVBoxLayout(summary_gb)
        sg_lay.setSpacing(4)

        self._lbl_max = QLabel("Highest Temp: -")
        self._lbl_min = QLabel("Lowest Temp: -")
        self._lbl_mean = QLabel("Mean Body Temp: -")
        for lbl in (self._lbl_max, self._lbl_min, self._lbl_mean):
            lbl.setStyleSheet("color:#94a3b8;font-size:10px;font-weight:bold;")
            sg_lay.addWidget(lbl)

        sb_lay.addWidget(summary_gb)
        f_lay.addWidget(sidebar, stretch=2)

        self.stack.addWidget(focus_page)
        root_lay.addWidget(self.stack, stretch=1)

        self._set_focus_person(1)

    def update_readings(self, nodes: Dict[int, Reading]):
        self._nodes = nodes
        for card in self.cards:
            card.update_readings(nodes)

        self.canvas.update_readings(nodes)
        self.update_stats()

    def _on_view_mode_changed(self, index: int):
        val = self.combo_mode.itemData(index)
        if val == 0:
            self.stack.setCurrentIndex(0)
        else:
            self.stack.setCurrentIndex(1)
            self._set_focus_person(val)

    def _inspect_person(self, person_id: int):
        idx = self.combo_mode.findData(person_id)
        if idx >= 0:
            self.combo_mode.setCurrentIndex(idx)

    def _set_focus_person(self, person_id: int):
        self.current_person_id = person_id
        p = self.persons[person_id - 1]
        self.input_pname.blockSignals(True)
        self.input_pname.setText(p.name)
        self.input_pname.blockSignals(False)

        self.canvas.set_mapping(p.mapping)

        for bp in BODY_PARTS:
            combo = self._combos[bp.id]
            combo.blockSignals(True)
            curr_node = p.mapping.get(bp.id)
            c_idx = combo.findData(curr_node)
            if c_idx >= 0:
                combo.setCurrentIndex(c_idx)
            combo.blockSignals(False)

        self.update_stats()

    def _on_person_name_changed(self, text: str):
        p = self.persons[self.current_person_id - 1]
        p.name = text.strip() or f"Subject {self.current_person_id:02d}"
        save_multi_person_config(self.persons)

        c_idx = self.combo_mode.findData(self.current_person_id)
        if c_idx >= 0:
            self.combo_mode.setItemText(c_idx, f"👤 {p.name}")

        self.cards[self.current_person_id - 1].update_readings(self._nodes)

    def update_stats(self):
        p = self.persons[self.current_person_id - 1]
        valid_temps: List[Tuple[str, int, float]] = []
        for bp in BODY_PARTS:
            nid = p.mapping.get(bp.id)
            if nid is not None and nid in self._nodes:
                r = self._nodes[nid]
                if r.temperature is not None:
                    valid_temps.append((bp.short_name, nid, r.temperature))

        if not valid_temps:
            self._lbl_max.setText("Highest Temp: -")
            self._lbl_min.setText("Lowest Temp: -")
            self._lbl_mean.setText("Mean Body Temp: -")
            return

        max_t = max(valid_temps, key=lambda x: x[2])
        min_t = min(valid_temps, key=lambda x: x[2])
        avg_t = sum(x[2] for x in valid_temps) / len(valid_temps)

        self._lbl_max.setText(f"Highest: {max_t[0]} (N{max_t[1]:02d}) -> {max_t[2]:.1f} °C")
        self._lbl_min.setText(f"Lowest:  {min_t[0]} (N{min_t[1]:02d}) -> {min_t[2]:.1f} °C")
        self._lbl_mean.setText(f"Mean Body Temp: {avg_t:.1f} °C ({len(valid_temps)} zones)")

    def _on_combo_changed(self, part_id: int, index: int):
        combo = self._combos[part_id]
        node_id = combo.itemData(index)

        p = self.persons[self.current_person_id - 1]
        p.mapping[part_id] = node_id

        save_multi_person_config(self.persons)
        self.canvas.set_mapping(p.mapping)
        self.cards[self.current_person_id - 1].update_readings(self._nodes)
        self.update_stats()

    def _on_part_clicked_canvas(self, part_id: int):
        for bpid, widget in self._row_widgets.items():
            if bpid == part_id:
                widget.setStyleSheet("QWidget{background:#0e639c;border-radius:5px;padding:2px;}")
            else:
                widget.setStyleSheet("QWidget{background:#1c1f2e;border-radius:5px;padding:2px;}")

    def _reset_default_mapping(self):
        p = self.persons[self.current_person_id - 1]
        offset = (self.current_person_id - 1) * 19
        for bp in BODY_PARTS:
            target_node = bp.id + offset
            combo = self._combos[bp.id]
            idx = combo.findData(target_node)
            if idx >= 0:
                combo.setCurrentIndex(idx)

    def _clear_all_mapping(self):
        for bp in BODY_PARTS:
            combo = self._combos[bp.id]
            idx = combo.findData(None)
            if idx >= 0:
                combo.setCurrentIndex(idx)


_TABLE_COLUMNS = ["Node", "Status", "Temp (C)", "Humidity (%)", "Seq", "Flags", "Last seen"]


class MainWindow(QMainWindow):
    def __init__(self, config: dict):
        super().__init__()
        self._config = config
        self.setWindowTitle("PAwR Medical Thermography Dashboard")
        self.setMinimumSize(1020, 660)
        self.resize(1260, 800)

        self._nodes: Dict[int, Reading] = {}
        self._row_for_node: Dict[int, int] = {}
        self._selected_node: Optional[int] = None

        self._db = sensor_db.open_db()
        self._build_ui()
        self._start_mqtt()

        self._health_timer = QTimer(self)
        self._health_timer.timeout.connect(self._on_timer_tick)
        self._health_timer.start(1000)

    def _build_ui(self):
        root = QWidget()
        root.setStyleSheet("background:#121316;")
        self.setCentralWidget(root)
        vlay = QVBoxLayout(root)
        vlay.setContentsMargins(0, 0, 0, 0)
        vlay.setSpacing(0)

        # Premium Header Bar
        bar = QWidget()
        bar.setStyleSheet("background:#161822;border-bottom:1px solid #2d3245;")
        bar.setFixedHeight(48)
        blay = QHBoxLayout(bar)
        blay.setContentsMargins(12, 0, 12, 0)

        logo_lbl = QLabel("🔴 LIVE")
        logo_lbl.setFont(QFont("Segoe UI", 9, QFont.Bold))
        logo_lbl.setStyleSheet("color:#ef4444;background:#2d1a1a;padding:3px 8px;border-radius:4px;")
        blay.addWidget(logo_lbl)

        title = QLabel("PAwR Medical Thermography Dashboard")
        title.setFont(QFont("Segoe UI", 12, QFont.Bold))
        title.setStyleSheet("color:#f0f2f5;")
        blay.addWidget(title)

        blay.addStretch()

        snap_btn = QPushButton("📸 Save Map PNG...")
        snap_btn.setStyleSheet(
            "QPushButton{background:#0e639c;color:white;border:1px solid #00e5ff;"
            "padding:5px 12px;border-radius:5px;font-weight:bold;font-size:11px;}"
            "QPushButton:hover{background:#00e5ff;color:black;}"
        )
        snap_btn.clicked.connect(self._save_map_image)
        blay.addWidget(snap_btn)

        blay.addSpacing(8)

        exp_btn = QPushButton("Export CSV...")
        exp_btn.setStyleSheet(
            "QPushButton{background:#222533;color:#eee;border:1px solid #3b4259;"
            "padding:5px 12px;border-radius:5px;font-weight:bold;font-size:11px;}"
            "QPushButton:hover{background:#2d3345;color:white;}"
        )
        exp_btn.clicked.connect(self._export_csv)
        blay.addWidget(exp_btn)
        vlay.addWidget(bar)

        # Tab Widget Styling
        self._tabs = QTabWidget()
        self._tabs.setStyleSheet(
            "QTabWidget::pane { border: 1px solid #2d3245; background: #121316; border-radius: 8px; }"
            "QTabBar::tab { background: #1a1c24; color: #94a3b8; padding: 8px 24px; font-weight: bold; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 4px; border: 1px solid #2d3245; border-bottom: none; }"
            "QTabBar::tab:selected { background: #0e639c; color: #ffffff; border-bottom: 2px solid #00e5ff; }"
            "QTabBar::tab:hover { background: #252836; color: #ffffff; }"
        )

        # Tab 1: Table & History
        tab_table = QWidget()
        tab_table.setStyleSheet("background:#121316;")
        clay = QHBoxLayout(tab_table)
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
            "QTableWidget{background:#161822;color:#f0f2f5;border-radius:10px;"
            "gridline-color:#2d3245;border:1px solid #2d3245;}"
            "QHeaderView::section{background:#222533;color:#38bdf8;"
            "border:none;padding:6px;font-weight:bold;}"
        )
        clay.addWidget(self._table, stretch=2)

        self._chart = HistoryChart(self._db)
        self._chart.setStyleSheet("background:#161822;border-radius:10px;border:1px solid #2d3245;")
        clay.addWidget(self._chart, stretch=3)

        self._tabs.addTab(tab_table, "📊 Table & History")

        # Tab 2: Body Silhouette
        self._body_tab = BodyMapTab()
        self._tabs.addTab(self._body_tab, "👥 Multi-Person Body Maps")

        vlay.addWidget(self._tabs, stretch=1)

        # Status Bar Styling
        sbar = QWidget()
        sbar.setFixedHeight(30)
        sbar.setStyleSheet("background:#101114;border-top:1px solid #252836;")
        slay = QHBoxLayout(sbar)
        slay.setContentsMargins(12, 0, 12, 0)
        slay.setSpacing(20)
        self._conn_lbl = QLabel("Connecting...")
        self._health_lbl = QLabel("🟢 Active: 0  🟡 Idle: 0  🔴 Offline: 0")
        self._mean_t_lbl = QLabel("Mean Temp: -")
        self._mean_h_lbl = QLabel("Mean Hum: -")
        self._db_lbl = QLabel(f"DB: {sensor_db.DB_PATH.name}")

        for lbl in [self._conn_lbl, self._health_lbl, self._mean_t_lbl, self._mean_h_lbl, self._db_lbl]:
            lbl.setStyleSheet("color:#94a3b8;font-size:10px;font-weight:bold;")
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
        self._body_tab.update_readings(self._nodes)

    def _on_timer_tick(self):
        self._refresh_table()
        self._update_stats()
        self._body_tab.update_readings(self._nodes)

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
            health = r.health()

            node_item = QTableWidgetItem(str(node_id))
            status_item = QTableWidgetItem(f"🟢 Active" if health == "ACTIVE" else (f"🟡 Idle" if health == "IDLE" else f"🔴 Offline"))
            temp_item = QTableWidgetItem(f"{r.temperature:.2f}" if r.temperature is not None else "-")
            hum_item = QTableWidgetItem(f"{r.humidity:.1f}" if r.humidity is not None else "-")
            seq_item = QTableWidgetItem(str(r.seq) if r.seq is not None else "-")
            flags_item = QTableWidgetItem(self._format_flags(r.flags))
            seen_item = QTableWidgetItem(r.last_seen.strftime("%H:%M:%S") if r.last_seen else "-")

            temp_item.setForeground(temp_color(r.temperature))

            for item in (node_item, status_item, temp_item, hum_item, seq_item, flags_item, seen_item):
                item.setTextAlignment(Qt.AlignCenter)

            for col, item in enumerate(
                (node_item, status_item, temp_item, hum_item, seq_item, flags_item, seen_item)
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

        active_cnt = sum(1 for r in self._nodes.values() if r.health() == "ACTIVE")
        idle_cnt = sum(1 for r in self._nodes.values() if r.health() == "IDLE")
        off_cnt = sum(1 for r in self._nodes.values() if r.health() == "OFFLINE")

        self._health_lbl.setText(f"🟢 Active: {active_cnt}  🟡 Idle: {idle_cnt}  🔴 Offline: {off_cnt}")
        self._mean_t_lbl.setText(
            f"Mean Temp: {sum(temps)/len(temps):.1f} C" if temps else "Mean Temp: -"
        )
        self._mean_h_lbl.setText(
            f"Mean Hum: {sum(hums)/len(hums):.1f} %" if hums else "Mean Hum: -"
        )

    def _save_map_image(self):
        default_name = f"multi_person_thermals_{datetime.now().strftime('%Y%m%d_%H%M%S')}.png"
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Multi-Person Thermal Map PNG", default_name, "PNG Images (*.png);;All Files (*)"
        )
        if not path:
            return

        pixmap = self._body_tab.grab()
        if pixmap.save(path, "PNG"):
            QMessageBox.information(
                self, "Image Saved", f"Multi-person thermal map saved to:\n{path}"
            )
        else:
            QMessageBox.warning(self, "Save Failed", "Could not save PNG image file.")

    def _export_csv(self):
        default_name = f"multi_person_export_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        path, _ = QFileDialog.getSaveFileName(
            self, "Export CSV", default_name, "CSV files (*.csv)"
        )
        if not path:
            return

        rows = sensor_db.query_all_for_export(self._db)
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["datetime", "nodeId", "seq", "flags", "temperature", "humidity"])
            for r in rows:
                w.writerow([r[0], r[1], r[2], r[3], r[4], r[5]])
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
    pal.setColor(QPalette.Window, QColor(18, 19, 24))
    pal.setColor(QPalette.WindowText, QColor(240, 242, 245))
    pal.setColor(QPalette.Base, QColor(22, 25, 35))
    pal.setColor(QPalette.AlternateBase, QColor(28, 31, 44))
    pal.setColor(QPalette.Text, QColor(240, 242, 245))
    pal.setColor(QPalette.Button, QColor(37, 40, 54))
    pal.setColor(QPalette.ButtonText, QColor(240, 242, 245))
    pal.setColor(QPalette.Highlight, QColor(14, 99, 156))
    pal.setColor(QPalette.HighlightedText, Qt.white)
    app.setPalette(pal)

    win = MainWindow(config)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
