"""
node_manager_gui.py -- PyQt5 operator console for node lifecycle management:
scan for nodes, connect (INIT) new ones, download stored data from inited
ones, and track when each node was last downloaded from.

This is a different tool from sensor_gui.py (that one's a live MQTT/UART
telemetry dashboard). This one drives central's firmware over its USB
console using the line-based command protocol added 2026-08-14 (see
central/src/main.c's file header): SCAN / INIT <node_id> / DOWNLOAD
<node_id> / STOP, with structured "EVT ..." result lines. All BLE logic
stays on central's already-proven firmware -- this GUI only ever talks
over one serial port, never touches BLE directly (explicit architecture
choice: reuse central's tested GATT code instead of a second BLE stack in
Python).

"Last downloaded" timestamps are wall-clock and persisted locally in
node_manager.db (see node_manager_db.py) -- central/nodes have no RTC (see
common/pawr_protocol.h's file header), so wall-clock time is only ever
meaningful on this PC, not on the firmware side. Central's own per-node
state is in-memory only and resets on reboot; this GUI's DB is the
durable record of download history across central restarts.

Requirements:
    pip install -r requirements.txt

Usage:
    python node_manager_gui.py
"""

import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, Optional

from PyQt5.QtCore import QObject, Qt, QThread, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QFont
from PyQt5.QtWidgets import (
    QApplication, QComboBox, QDialog, QFileDialog, QFrame, QHBoxLayout, QHeaderView,
    QLabel, QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton,
    QSizePolicy, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

import serial
import serial.tools.list_ports

import node_manager_db

try:
    from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg
    from matplotlib.figure import Figure
    _MPL = True
except ImportError:
    _MPL = False

# Seeed XIAO nRF52840's USB-CDC-ACM VID:PID -- both central and peripheral
# nodes use this exact board, so this narrows the port dropdown to
# candidates but can't tell central apart from a node also plugged in over
# USB (both enumerate identically). The operator picks the right one from
# the dropdown; there is no way to auto-distinguish them from the USB
# descriptor alone (confirmed 2026-08-14: same VID:PID, same generic
# "Serielles USB-Geraet" description, on both a central and a node board
# connected simultaneously).
XIAO_VID_PID = "2FE3:0004"

EVT_LINE_RE = re.compile(r"^EVT (\S+)(.*)$")
PROSE_HEADER_RE = re.compile(r"Download header:\s*(\d+)\s*entries", re.IGNORECASE)

COLOR_BG = "#0b1326"
COLOR_SURFACE = "#171f33"
COLOR_SURFACE_LOW = "#131b2e"
COLOR_SURFACE_HIGH = "#222a3d"
COLOR_SURFACE_HIGHEST = "#2d3449"
COLOR_OUTLINE = "#3b494c"
COLOR_OUTLINE_VARIANT = "#2d3245"
COLOR_TEXT = "#dae2fd"
COLOR_TEXT_DIM = "#8a93a6"
COLOR_PRIMARY = "#00e5ff"       # cyan -- links, focus, connecting
COLOR_SECONDARY = "#4edea3"     # green -- healthy/inited/connected
COLOR_TERTIARY = "#ffb95f"      # amber -- warnings, downloading
COLOR_PURPLE = "#c084fc"        # purple -- charts, telemetry
COLOR_ERROR = "#ff6b6b"

# Modern icon glyphs
ICON_DASHBOARD = "▤"
ICON_USB = "⬡"
ICON_REFRESH = "↻"
ICON_SCAN = "◎"
ICON_INIT = "⚡"
ICON_DOWNLOAD = "⬇"
ICON_GRAPH = "📊"
ICON_EXPORT = "📤"
ICON_BUSY = "⏳"

APP_STYLESHEET = f"""
QWidget {{
    background: {COLOR_BG};
    color: {COLOR_TEXT};
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    font-size: 13px;
}}
QMainWindow {{
    background: {COLOR_BG};
}}
#Sidebar {{
    background: {COLOR_SURFACE_HIGH};
    border-right: 1px solid {COLOR_OUTLINE_VARIANT};
}}
#SidebarTitle {{
    color: {COLOR_TEXT};
    font-size: 16px;
    font-weight: 700;
    letter-spacing: 0.5px;
}}
#SidebarSubtitle {{
    color: {COLOR_TEXT_DIM};
    font-size: 11px;
}}
QPushButton#NavButton {{
    background: transparent;
    color: {COLOR_TEXT_DIM};
    border: none;
    border-left: 3px solid transparent;
    text-align: left;
    padding: 8px 12px;
    font-size: 13px;
}}
QPushButton#NavButton:hover {{
    background: {COLOR_SURFACE_LOW};
    color: {COLOR_TEXT};
}}
QPushButton#NavButton[active="true"] {{
    background: {COLOR_SURFACE_HIGHEST};
    color: {COLOR_SECONDARY};
    border-left: 3px solid {COLOR_SECONDARY};
    font-weight: 700;
}}
#TopBar {{
    background: {COLOR_SURFACE_LOW};
    border-bottom: 1px solid {COLOR_OUTLINE_VARIANT};
}}
#TopBarTitle {{
    color: {COLOR_PRIMARY};
    font-size: 16px;
    font-weight: 700;
    letter-spacing: 1px;
}}
#PortBox {{
    background: {COLOR_SURFACE};
    border: 1px solid {COLOR_OUTLINE_VARIANT};
    border-radius: 6px;
}}
QComboBox {{
    background: transparent;
    border: none;
    color: {COLOR_TEXT};
    padding: 3px 8px;
    font-weight: 600;
}}
QPushButton#PortRefreshBtn {{
    background: transparent;
    color: {COLOR_TEXT_DIM};
    border: none;
    padding: 4px 8px;
    font-size: 14px;
}}
QPushButton#PortRefreshBtn:hover {{
    color: {COLOR_PRIMARY};
}}
QPushButton#ScanButton {{
    background: rgba(0, 229, 255, 0.08);
    border: 1px solid {COLOR_PRIMARY};
    color: {COLOR_PRIMARY};
    border-radius: 6px;
    padding: 6px 16px;
    font-weight: 600;
}}
QPushButton#ScanButton:hover {{
    background: rgba(0, 229, 255, 0.18);
}}
QPushButton#ScanButton:disabled {{
    border-color: {COLOR_OUTLINE};
    color: {COLOR_TEXT_DIM};
    background: transparent;
}}
QPushButton#ConnectButton {{
    background: {COLOR_PRIMARY};
    color: #00363d;
    border: none;
    border-radius: 6px;
    padding: 6px 20px;
    font-weight: 700;
}}
QPushButton#ConnectButton:hover {{
    background: #5df0ff;
}}
QPushButton#ConnectButton[connected="true"] {{
    background: #2a3142;
    color: {COLOR_ERROR};
    border: 1px solid rgba(255, 107, 107, 0.3);
}}
QPushButton#ConnectButton[connected="true"]:hover {{
    background: rgba(255, 107, 107, 0.15);
    border-color: {COLOR_ERROR};
}}
#StatCard {{
    background: {COLOR_SURFACE};
    border: 1px solid {COLOR_OUTLINE_VARIANT};
    border-radius: 8px;
}}
#StatCard[accent="true"] {{
    border-top: 2px solid {COLOR_SECONDARY};
}}
#StatLabel {{
    color: {COLOR_TEXT_DIM};
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 1px;
}}
#StatValue {{
    color: {COLOR_TEXT};
    font-size: 24px;
    font-weight: 700;
}}
#StatValueAccent {{
    color: {COLOR_SECONDARY};
    font-size: 24px;
    font-weight: 700;
}}
#PanelHeader {{
    background: {COLOR_SURFACE_LOW};
    border-bottom: 1px solid {COLOR_OUTLINE_VARIANT};
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
}}
#PanelTitle {{
    color: {COLOR_TEXT};
    font-size: 14px;
    font-weight: 700;
}}
QTableWidget {{
    background: {COLOR_SURFACE};
    border: 1px solid {COLOR_OUTLINE_VARIANT};
    border-top: none;
    gridline-color: {COLOR_OUTLINE_VARIANT};
}}
QHeaderView::section {{
    background: {COLOR_SURFACE_HIGHEST};
    color: {COLOR_TEXT_DIM};
    border: none;
    border-bottom: 1px solid {COLOR_OUTLINE_VARIANT};
    padding: 8px 6px;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.8px;
}}
QTableWidget::item {{
    padding: 4px 6px;
}}
QTableWidget::item:selected {{
    background: {COLOR_SURFACE_HIGH};
    color: {COLOR_TEXT};
}}

/* Action Button Styles */
QPushButton#BtnInit {{
    background: rgba(0, 229, 255, 0.08);
    color: {COLOR_PRIMARY};
    border: 1px solid rgba(0, 229, 255, 0.35);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 11px;
    font-weight: 600;
}}
QPushButton#BtnInit:hover {{
    background: rgba(0, 229, 255, 0.22);
    border-color: {COLOR_PRIMARY};
}}
QPushButton#BtnInit:disabled {{
    background: transparent;
    color: #404859;
    border-color: #242c3d;
}}

QPushButton#BtnDownload {{
    background: rgba(0, 229, 255, 0.9);
    color: #003038;
    border: 1px solid #5df0ff;
    border-radius: 4px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 700;
}}
QPushButton#BtnDownload:hover {{
    background: #5df0ff;
}}
QPushButton#BtnDownload:disabled {{
    background: #182030;
    color: #404859;
    border-color: #242c3d;
}}

QPushButton#BtnGraph {{
    background: rgba(192, 132, 252, 0.12);
    color: {COLOR_PURPLE};
    border: 1px solid rgba(192, 132, 252, 0.35);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 11px;
    font-weight: 600;
}}
QPushButton#BtnGraph:hover {{
    background: rgba(192, 132, 252, 0.24);
    border-color: {COLOR_PURPLE};
    color: #e9d5ff;
}}
QPushButton#BtnGraph:disabled {{
    background: transparent;
    color: #404859;
    border-color: #242c3d;
}}

QPushButton#BtnExport {{
    background: rgba(218, 226, 253, 0.08);
    color: {COLOR_TEXT};
    border: 1px solid rgba(218, 226, 253, 0.25);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 11px;
    font-weight: 600;
}}
QPushButton#BtnExport:hover {{
    background: rgba(218, 226, 253, 0.18);
    border-color: {COLOR_TEXT};
}}
QPushButton#BtnExport:disabled {{
    background: transparent;
    color: #404859;
    border-color: #242c3d;
}}

#Terminal {{
    background: #050914;
    border: 1px solid {COLOR_OUTLINE_VARIANT};
    border-radius: 8px;
}}
#TerminalHeader {{
    background: {COLOR_SURFACE_LOW};
    color: {COLOR_TEXT_DIM};
    border-bottom: 1px solid {COLOR_OUTLINE_VARIANT};
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
}}
QPlainTextEdit#TerminalOutput {{
    background: #050914;
    color: {COLOR_TEXT_DIM};
    border: none;
    font-family: 'Consolas', 'JetBrains Mono', monospace;
    font-size: 11px;
}}
#Footer {{
    background: {COLOR_SURFACE_LOW};
    border-top: 1px solid {COLOR_OUTLINE_VARIANT};
    color: {COLOR_OUTLINE};
    font-family: 'Consolas', monospace;
    font-size: 11px;
}}

QProgressBar {{
    background: #0d1627;
    border: 1px solid #232d42;
    border-radius: 3px;
    height: 6px;
    text-align: center;
}}
QProgressBar::chunk {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00b4d8, stop:1 #00e5ff);
    border-radius: 2px;
}}
"""


def _state_pill_style(state: str) -> str:
    """Returns an inline style string for a QLabel acting as a colored
    status pill, matching the mockup's rounded-badge look per fw_state."""
    if state == "DOWNLOADED":
        fg, bg, border = COLOR_SECONDARY, "rgba(78, 222, 163, 0.12)", "rgba(78, 222, 163, 0.3)"
    elif state == "INITED":
        fg, bg, border = COLOR_PRIMARY, "rgba(0, 229, 255, 0.12)", "rgba(0, 229, 255, 0.3)"
    else:
        fg, bg, border = COLOR_TEXT_DIM, "rgba(255, 255, 255, 0.04)", COLOR_OUTLINE_VARIANT
    return (
        f"QLabel {{ color: {fg}; background: {bg}; border: 1px solid {border};"
        " border-radius: 8px; padding: 2px 8px; font-size: 11px; font-weight: 600; }}"
    )


def find_candidate_ports():
    """Every currently-connected port matching the XIAO's VID:PID, plus
    (if none matched -- e.g. a different board revision) every port at
    all, so the dropdown is never empty just because the heuristic missed."""
    ports = list(serial.tools.list_ports.comports())
    matches = [p.device for p in ports if XIAO_VID_PID.lower() in (p.hwid or "").lower()]
    return matches if matches else [p.device for p in ports]


@dataclass
class NodeInfo:
    node_id: int
    rssi: Optional[int] = None
    fw_state: str = "UNKNOWN"
    last_seen: float = field(default_factory=time.time)
    connection_state: str = "idle"  # idle | connecting | connected | downloading
    download_count: int = 0
    download_total: int = 0
    last_downloaded_at: Optional[str] = None
    last_entries_count: Optional[int] = None


class CentralSerialWorker(QThread):
    """Owns the serial port: reads lines, parses EVT records, emits Qt
    signals. Commands are queued from the GUI thread and written from
    within this thread's run() loop so port access never crosses threads.
    """

    node_seen = pyqtSignal(int, int, str)          # node_id, rssi, fw_state
    node_connecting = pyqtSignal(int)
    node_connected = pyqtSignal(int)
    node_disconnected = pyqtSignal(int)
    init_ok = pyqtSignal(int)
    download_header_seen = pyqtSignal(int, int)     # node_id, total entries
    download_ok = pyqtSignal(int, int)              # node_id, entries
    download_progress = pyqtSignal(int, int, int)   # node_id, count so far, total entries
    # node_id, seq, millis_since_init, temp_cdeg, humidity_pct10
    download_reading = pyqtSignal(int, int, int, int, int)
    node_error = pyqtSignal(int, str)
    raw_line = pyqtSignal(str)
    port_status = pyqtSignal(str)                   # human-readable connection status

    def __init__(self, port: str):
        super().__init__()
        self._port_name = port
        self._ser: Optional[serial.Serial] = None
        self._running = True
        self._pending_commands = []
        self._download_counts: Dict[int, int] = {}
        self._download_totals: Dict[int, int] = {}
        self._current_target_node: Optional[int] = None

    def send_command(self, line: str) -> None:
        parts = line.strip().split()
        if len(parts) >= 2 and parts[0] in ("INIT", "DOWNLOAD"):
            try:
                self._current_target_node = int(parts[1])
            except ValueError:
                pass
        self._pending_commands.append(line)

    def stop(self) -> None:
        self._running = False

    def _open_port(self) -> bool:
        for attempt in range(5):
            if not self._running:
                return False
            try:
                self._ser = serial.Serial(self._port_name, 115200, timeout=0.2)
                return True
            except (PermissionError, serial.SerialException) as e:
                self.port_status.emit(f"Opening {self._port_name} (attempt {attempt + 1}/5): {e}")
                time.sleep(1)
        return False

    def run(self) -> None:
        if not self._open_port():
            self.port_status.emit(f"Failed to open {self._port_name}")
            return

        self.port_status.emit(f"Connected to {self._port_name}")

        while self._running:
            while self._pending_commands:
                cmd = self._pending_commands.pop(0)
                try:
                    self._ser.write((cmd + "\n").encode())
                except serial.SerialException as e:
                    self.port_status.emit(f"Write failed: {e}")

            try:
                raw = self._ser.readline()
            except serial.SerialException as e:
                self.port_status.emit(f"Read failed: {e}")
                time.sleep(1)
                continue

            if not raw:
                continue

            line = raw.decode(errors="replace").rstrip("\r\n")
            if not line:
                continue

            self.raw_line.emit(line)
            self._handle_line(line)

        if self._ser and self._ser.is_open:
            self._ser.close()

    def _handle_line(self, line: str) -> None:
        # Check prose header line from central: "Download header: %u entries"
        m_hdr = PROSE_HEADER_RE.search(line)
        if m_hdr and self._current_target_node is not None:
            total = int(m_hdr.group(1))
            self._download_totals[self._current_target_node] = total
            self.download_header_seen.emit(self._current_target_node, total)

        m = EVT_LINE_RE.match(line)
        if not m:
            return

        kind = m.group(1)
        rest = m.group(2).strip()
        fields = dict(zip(rest.split()[::2], rest.split()[1::2]))
        node_id = int(fields["NODE"]) if "NODE" in fields else None
        if node_id is not None:
            self._current_target_node = node_id

        if kind == "SCAN" and node_id is not None:
            rssi = int(fields.get("RSSI", "0"))
            state = fields.get("STATE", "UNKNOWN")
            self.node_seen.emit(node_id, rssi, state)
        elif kind == "CONNECTING" and node_id is not None:
            self.node_connecting.emit(node_id)
        elif kind == "CONNECTED" and node_id is not None:
            self.node_connected.emit(node_id)
        elif kind == "DISCONNECTED" and node_id is not None:
            self.node_disconnected.emit(node_id)
        elif kind == "INIT_OK" and node_id is not None:
            self.init_ok.emit(node_id)
        elif kind == "DOWNLOAD_HEADER" and node_id is not None:
            total = int(fields.get("TOTAL", fields.get("ENTRIES", "0")))
            self._download_totals[node_id] = total
            self.download_header_seen.emit(node_id, total)
        elif kind == "DOWNLOAD_DATA" and node_id is not None:
            count = self._download_counts.get(node_id, 0) + 1
            self._download_counts[node_id] = count
            total = self._download_totals.get(node_id, 0)
            self.download_progress.emit(node_id, count, total)
            try:
                seq = int(fields.get("SEQ", "0"))
                ms = int(fields.get("MS", "0"))
                temp_cdeg = int(fields.get("TEMP", "0"))
                hum = int(fields.get("HUM", "0"))
            except ValueError:
                pass
            else:
                self.download_reading.emit(node_id, seq, ms, temp_cdeg, hum)
        elif kind == "DOWNLOAD_OK" and node_id is not None:
            entries = int(fields.get("ENTRIES", "0"))
            self._download_counts.pop(node_id, None)
            self._download_totals.pop(node_id, None)
            self.download_ok.emit(node_id, entries)
        elif kind == "ERROR" and node_id is not None:
            msg_match = re.search(r'MSG "([^"]*)"', rest)
            msg = msg_match.group(1) if msg_match else "unknown error"
            self._download_counts.pop(node_id, None)
            self._download_totals.pop(node_id, None)
            self.node_error.emit(node_id, msg)


class StatusCellWidget(QWidget):
    """Persistent, in-place status cell widget that smoothly displays
    connection state and download progress without tearing down the widget."""

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(6, 4, 6, 4)
        lay.setSpacing(3)

        self.label = QLabel("idle")
        self.label.setStyleSheet(f"color: {COLOR_TEXT_DIM}; font-size: 11px;")
        lay.addWidget(self.label)

        self.progress_bar = QProgressBar()
        self.progress_bar.setFixedHeight(6)
        self.progress_bar.setTextVisible(False)
        self.progress_bar.hide()
        lay.addWidget(self.progress_bar)

    def set_idle(self):
        self.label.setText("idle")
        self.label.setStyleSheet(f"color: {COLOR_TEXT_DIM}; font-size: 11px;")
        self.progress_bar.hide()

    def set_connecting(self):
        self.label.setText("CONNECTING...")
        self.label.setStyleSheet(f"color: {COLOR_TERTIARY}; font-size: 11px; font-weight: 600;")
        self.progress_bar.hide()

    def set_connected(self):
        self.label.setText("CONNECTED")
        self.label.setStyleSheet(f"color: {COLOR_SECONDARY}; font-size: 11px; font-weight: 600;")
        self.progress_bar.hide()

    def set_downloading(self, count: int, total: int):
        if total > 0:
            pct = int(min(100, (count / total) * 100))
            self.label.setText(f"{ICON_BUSY} DOWNLOADING: {count}/{total} ({pct}%)")
            self.progress_bar.setRange(0, total)
            self.progress_bar.setValue(count)
        else:
            self.label.setText(f"{ICON_BUSY} DOWNLOADING: {count} entries")
            self.progress_bar.setRange(0, max(count, 100))
            self.progress_bar.setValue(count)

        self.label.setStyleSheet(f"color: {COLOR_PRIMARY}; font-size: 11px; font-weight: 700;")
        if not self.progress_bar.isVisible():
            self.progress_bar.show()

    def set_synced(self, count: int):
        self.label.setText(f"SYNC COMPLETE ({count} entries)")
        self.label.setStyleSheet(f"color: {COLOR_SECONDARY}; font-size: 11px; font-weight: 600;")
        self.progress_bar.hide()


class ActionCellWidget(QWidget):
    """Persistent, styled action buttons container with clear labels,
    centered alignment, modern hover/disabled states, and clean spacing."""

    def __init__(self, node_id: int, on_init, on_download, on_graph, on_export, parent=None):
        super().__init__(parent)
        self.node_id = node_id
        lay = QHBoxLayout(self)
        lay.setContentsMargins(6, 2, 6, 2)
        lay.setSpacing(6)
        lay.setAlignment(Qt.AlignCenter)

        self.init_btn = QPushButton(f"{ICON_INIT} Init")
        self.init_btn.setObjectName("BtnInit")
        self.init_btn.setToolTip(f"Initialize node {node_id} measurement session")
        self.init_btn.setCursor(Qt.PointingHandCursor)
        self.init_btn.clicked.connect(lambda: on_init(self.node_id))
        lay.addWidget(self.init_btn)

        self.download_btn = QPushButton(f"{ICON_DOWNLOAD} Download")
        self.download_btn.setObjectName("BtnDownload")
        self.download_btn.setToolTip(f"Sync & download stored readings from node {node_id}")
        self.download_btn.setCursor(Qt.PointingHandCursor)
        self.download_btn.clicked.connect(lambda: on_download(self.node_id))
        lay.addWidget(self.download_btn)

        self.graph_btn = QPushButton(f"{ICON_GRAPH} Graph")
        self.graph_btn.setObjectName("BtnGraph")
        self.graph_btn.setToolTip(f"View telemetry plot for node {node_id}")
        self.graph_btn.setCursor(Qt.PointingHandCursor)
        self.graph_btn.clicked.connect(lambda: on_graph(self.node_id))
        lay.addWidget(self.graph_btn)

        self.export_btn = QPushButton(f"{ICON_EXPORT} Export")
        self.export_btn.setObjectName("BtnExport")
        self.export_btn.setToolTip(f"Export node {node_id} data to CSV")
        self.export_btn.setCursor(Qt.PointingHandCursor)
        self.export_btn.clicked.connect(lambda: on_export(self.node_id))
        lay.addWidget(self.export_btn)

    def update_states(self, busy: bool, has_data: bool):
        self.init_btn.setEnabled(not busy)
        self.download_btn.setEnabled(not busy)
        self.graph_btn.setEnabled(has_data)
        self.export_btn.setEnabled(has_data)


class DownloadChartDialog(QDialog):
    """Plots one node's full downloaded log: temp/humidity vs.
    millis_since_init."""

    def __init__(self, db_conn, node_id: int, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"Node {node_id} -- Downloaded Data")
        self.resize(850, 520)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(16, 16, 16, 16)

        if not _MPL:
            layout.addWidget(QLabel("Install matplotlib to enable charts:\npip install matplotlib"))
            return

        rows = node_manager_db.query_readings(db_conn, node_id)
        if not rows:
            layout.addWidget(QLabel(f"No downloaded data stored for node {node_id} yet."))
            return

        fig = Figure(tight_layout=True)
        fig.patch.set_facecolor("#131b2e")
        canvas = FigureCanvasQTAgg(fig)
        layout.addWidget(canvas)

        seconds = [r[1] / 1000.0 for r in rows]
        temps = [r[2] / 100.0 if r[2] is not None else None for r in rows]
        hums = [r[3] / 10.0 if r[3] is not None else None for r in rows]

        ax1 = fig.add_subplot(111)
        ax1.set_facecolor("#171f33")
        ax1.set_xlabel("Time since init (s)", color="#dae2fd", fontsize=10, fontweight="bold")

        def filtered(xs, vs):
            pairs = [(x, v) for x, v in zip(xs, vs) if v is not None]
            return zip(*pairs) if pairs else ([], [])

        all_lines = []

        if any(v is not None for v in temps):
            tx, tv = filtered(seconds, temps)
            all_lines += ax1.plot(tx, tv, color="#ff5376", lw=1.8, label="Temperature (°C)")
            ax1.set_ylabel("Temperature (°C)", color="#ff5376", fontsize=10, fontweight="bold")
            ax1.tick_params(axis="y", labelcolor="#ff5376", labelsize=9)

        ax2 = ax1.twinx()
        if any(v is not None for v in hums):
            hx, hv = filtered(seconds, hums)
            all_lines += ax2.plot(hx, hv, color="#00e5ff", lw=1.8, linestyle="--", label="Humidity (%)")
            ax2.set_ylabel("Humidity (%)", color="#00e5ff", fontsize=10, fontweight="bold")
            ax2.tick_params(axis="y", labelcolor="#00e5ff", labelsize=9)

        if all_lines:
            ax1.legend(all_lines, [l.get_label() for l in all_lines],
                       loc="upper left", fontsize=9,
                       facecolor="#1c2438", edgecolor="#3b494c", labelcolor="white")

        ax1.tick_params(axis="x", labelsize=9, colors="#dae2fd")
        ax1.grid(True, color="#2d3245", linewidth=0.7, linestyle=":")
        for spine in list(ax1.spines.values()) + list(ax2.spines.values()):
            spine.set_color("#3b494c")

        canvas.draw_idle()


COL_NODE_ID = 0
COL_RSSI = 1
COL_FW_STATE = 2
COL_CONN_STATE = 3
COL_LAST_DOWNLOAD = 4
COL_ENTRIES = 5
COL_ACTIONS = 6
COLUMN_HEADERS = ["Node ID", "Signal", "State", "Status / Transfer", "Last Sync", "Data Entries", "Actions"]

STALE_NODE_TIMEOUT_S = 15.0


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("PAwR-NCS -- Node Lifecycle Manager")
        self.resize(1200, 780)

        self.db_conn = node_manager_db.open_db()
        self.nodes: Dict[int, NodeInfo] = {}
        for node_id, rec in node_manager_db.load_all(self.db_conn).items():
            self.nodes[node_id] = NodeInfo(
                node_id=node_id,
                last_downloaded_at=rec["last_downloaded_at"],
                last_entries_count=rec["entries_count"],
                last_seen=0.0,
            )

        self.worker: Optional[CentralSerialWorker] = None
        self._status_widgets: Dict[int, StatusCellWidget] = {}
        self._action_widgets: Dict[int, ActionCellWidget] = {}

        self._build_ui()
        self._refresh_table()

        self.stale_timer = QTimer(self)
        self.stale_timer.timeout.connect(self._check_stale_nodes)
        self.stale_timer.start(2000)

    def _build_ui(self):
        self.setStyleSheet(APP_STYLESHEET)
        self._scanning = False

        central = QWidget()
        root_row = QHBoxLayout(central)
        root_row.setContentsMargins(0, 0, 0, 0)
        root_row.setSpacing(0)

        root_row.addWidget(self._build_sidebar())

        right_col = QVBoxLayout()
        right_col.setContentsMargins(0, 0, 0, 0)
        right_col.setSpacing(0)
        right_col.addWidget(self._build_topbar())

        content = QWidget()
        content_lay = QVBoxLayout(content)
        content_lay.setContentsMargins(20, 20, 20, 20)
        content_lay.setSpacing(16)
        content_lay.addLayout(self._build_stat_cards())
        content_lay.addWidget(self._build_node_panel(), stretch=1)
        self.terminal_frame = self._build_terminal()
        content_lay.addWidget(self.terminal_frame)
        right_col.addWidget(content, stretch=1)

        right_col.addWidget(self._build_footer())

        right_widget = QWidget()
        right_widget.setLayout(right_col)
        root_row.addWidget(right_widget, stretch=1)

        self.setCentralWidget(central)

    def _build_sidebar(self) -> QWidget:
        sidebar = QWidget()
        sidebar.setObjectName("Sidebar")
        sidebar.setFixedWidth(220)
        lay = QVBoxLayout(sidebar)
        lay.setContentsMargins(16, 20, 16, 16)
        lay.setSpacing(4)

        title = QLabel("NODE_MGMT_01")
        title.setObjectName("SidebarTitle")
        lay.addWidget(title)
        self.sidebar_subtitle = QLabel("Not connected")
        self.sidebar_subtitle.setObjectName("SidebarSubtitle")
        lay.addWidget(self.sidebar_subtitle)
        lay.addSpacing(16)

        self.nav_dashboard_btn = QPushButton(f"{ICON_DASHBOARD}  Dashboard")
        self.nav_dashboard_btn.setObjectName("NavButton")
        self.nav_dashboard_btn.setProperty("active", "true")
        self.nav_dashboard_btn.setCursor(Qt.PointingHandCursor)
        lay.addWidget(self.nav_dashboard_btn)

        lay.addStretch()
        return sidebar

    def _build_topbar(self) -> QWidget:
        bar = QWidget()
        bar.setObjectName("TopBar")
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(24, 12, 24, 12)

        title = QLabel("CENTRAL FIRMWARE CONSOLE")
        title.setObjectName("TopBarTitle")
        lay.addWidget(title)
        lay.addStretch()

        port_box = QWidget()
        port_box.setObjectName("PortBox")
        port_lay = QHBoxLayout(port_box)
        port_lay.setContentsMargins(8, 2, 4, 2)
        port_lay.addWidget(QLabel(ICON_USB))
        self.port_combo = QComboBox()
        self.port_combo.addItems(find_candidate_ports())
        port_lay.addWidget(self.port_combo)
        refresh_ports_btn = QPushButton(ICON_REFRESH)
        refresh_ports_btn.setObjectName("PortRefreshBtn")
        refresh_ports_btn.setToolTip("Refresh COM ports")
        refresh_ports_btn.setCursor(Qt.PointingHandCursor)
        refresh_ports_btn.clicked.connect(self._refresh_ports)
        port_lay.addWidget(refresh_ports_btn)
        lay.addWidget(port_box)

        lay.addSpacing(16)

        self.scan_btn = QPushButton(f"{ICON_SCAN}  Global Scan")
        self.scan_btn.setObjectName("ScanButton")
        self.scan_btn.setCursor(Qt.PointingHandCursor)
        self.scan_btn.clicked.connect(self._toggle_scan)
        self.scan_btn.setEnabled(False)
        lay.addWidget(self.scan_btn)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("ConnectButton")
        self.connect_btn.setCursor(Qt.PointingHandCursor)
        self.connect_btn.clicked.connect(self._toggle_serial_connection)
        lay.addWidget(self.connect_btn)

        return bar

    def _build_stat_cards(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(16)

        self.stat_total_card, self.stat_total_value = self._make_stat_card(
            "TOTAL NODES DISCOVERED", "0", accent=False)
        self.stat_active_card, self.stat_active_value = self._make_stat_card(
            "ACTIVE CONNECTIONS", "0", accent=True)
        self.stat_latest_card, self.stat_latest_value = self._make_stat_card(
            "LATEST DOWNLOAD", "--", accent=False, mono=True)

        row.addWidget(self.stat_total_card)
        row.addWidget(self.stat_active_card)
        row.addWidget(self.stat_latest_card)
        return row

    def _make_stat_card(self, label_text: str, value_text: str, accent: bool, mono: bool = False):
        card = QFrame()
        card.setObjectName("StatCard")
        if accent:
            card.setProperty("accent", "true")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 12, 16, 12)
        label = QLabel(label_text)
        label.setObjectName("StatLabel")
        lay.addWidget(label)
        value = QLabel(value_text)
        value.setObjectName("StatValueAccent" if accent else "StatValue")
        if mono:
            value.setFont(QFont("Consolas", 13, QFont.Bold))
        lay.addWidget(value)
        return card, value

    def _build_node_panel(self) -> QWidget:
        panel = QFrame()
        panel.setObjectName("NodePanel")
        panel.setStyleSheet(
            f"#NodePanel {{ background: {COLOR_SURFACE}; border: 1px solid {COLOR_OUTLINE_VARIANT};"
            " border-radius: 8px; }}"
        )
        lay = QVBoxLayout(panel)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)

        header = QWidget()
        header.setObjectName("PanelHeader")
        hlay = QHBoxLayout(header)
        hlay.setContentsMargins(16, 12, 16, 12)
        title = QLabel("Discovered Cluster Nodes")
        title.setObjectName("PanelTitle")
        hlay.addWidget(title)
        hlay.addStretch()
        self.status_pill = QLabel("STATUS: IDLE")
        self.status_pill.setStyleSheet(_state_pill_style("UNKNOWN"))
        hlay.addWidget(self.status_pill)
        lay.addWidget(header)

        self.table = QTableWidget(0, len(COLUMN_HEADERS))
        self.table.setHorizontalHeaderLabels(COLUMN_HEADERS)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setShowGrid(False)
        self.table.verticalHeader().setDefaultSectionSize(48)
        self.table.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        # Set specific column widths & resize modes (no horizontal scrollbar needed)
        header_view = self.table.horizontalHeader()
        header_view.setSectionResizeMode(COL_NODE_ID, QHeaderView.Fixed)
        header_view.resizeSection(COL_NODE_ID, 85)

        header_view.setSectionResizeMode(COL_RSSI, QHeaderView.Fixed)
        header_view.resizeSection(COL_RSSI, 75)

        header_view.setSectionResizeMode(COL_FW_STATE, QHeaderView.Fixed)
        header_view.resizeSection(COL_FW_STATE, 110)

        header_view.setSectionResizeMode(COL_CONN_STATE, QHeaderView.Stretch)

        header_view.setSectionResizeMode(COL_LAST_DOWNLOAD, QHeaderView.Fixed)
        header_view.resizeSection(COL_LAST_DOWNLOAD, 150)

        header_view.setSectionResizeMode(COL_ENTRIES, QHeaderView.Fixed)
        header_view.resizeSection(COL_ENTRIES, 90)

        header_view.setSectionResizeMode(COL_ACTIONS, QHeaderView.Fixed)
        header_view.resizeSection(COL_ACTIONS, 305)

        lay.addWidget(self.table)
        return panel

    def _build_terminal(self) -> QWidget:
        frame = QFrame()
        frame.setObjectName("Terminal")
        frame.setFixedHeight(170)
        lay = QVBoxLayout(frame)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)

        header = QWidget()
        header.setObjectName("TerminalHeader")
        hlay = QHBoxLayout(header)
        hlay.setContentsMargins(12, 4, 12, 4)
        hlay.addWidget(QLabel("Serial Console Output"))
        hlay.addStretch()
        clear_btn = QPushButton("Clear")
        clear_btn.setObjectName("PortRefreshBtn")
        clear_btn.setCursor(Qt.PointingHandCursor)
        clear_btn.clicked.connect(lambda: self.terminal.clear())
        hlay.addWidget(clear_btn)
        lay.addWidget(header)

        self.terminal = QPlainTextEdit()
        self.terminal.setObjectName("TerminalOutput")
        self.terminal.setReadOnly(True)
        self.terminal.setMaximumBlockCount(500)
        lay.addWidget(self.terminal)

        return frame

    def _build_footer(self) -> QWidget:
        footer = QWidget()
        footer.setObjectName("Footer")
        footer.setFixedHeight(28)
        lay = QHBoxLayout(footer)
        lay.setContentsMargins(24, 0, 24, 0)
        lay.addWidget(QLabel("PAwR-NCS -- Node Lifecycle Manager"))
        lay.addStretch()
        self.footer_status = QLabel("○ Not connected")
        lay.addWidget(self.footer_status)
        return footer

    # ------------------------------------------------------------------
    # Serial connection lifecycle
    # ------------------------------------------------------------------

    def _refresh_ports(self):
        self.port_combo.clear()
        self.port_combo.addItems(find_candidate_ports())

    def _toggle_serial_connection(self):
        if self.worker is None:
            port = self.port_combo.currentText()
            if not port:
                QMessageBox.warning(self, "No port", "No serial port selected.")
                return
            self.worker = CentralSerialWorker(port)
            self.worker.node_seen.connect(self._on_node_seen)
            self.worker.node_connecting.connect(self._on_node_connecting)
            self.worker.node_connected.connect(self._on_node_connected)
            self.worker.node_disconnected.connect(self._on_node_disconnected)
            self.worker.init_ok.connect(self._on_init_ok)
            self.worker.download_header_seen.connect(self._on_download_header_seen)
            self.worker.download_ok.connect(self._on_download_ok)
            self.worker.download_progress.connect(self._on_download_progress)
            self.worker.download_reading.connect(self._on_download_reading)
            self.worker.node_error.connect(self._on_node_error)
            self.worker.port_status.connect(self._on_port_status)
            self.worker.raw_line.connect(self._on_raw_line)
            self.worker.start()
            self.connect_btn.setText("Disconnect")
            self.connect_btn.setProperty("connected", "true")
            self.connect_btn.setStyle(self.connect_btn.style())
            self.scan_btn.setEnabled(True)
            self._update_all_action_buttons()
        else:
            if self._scanning:
                self._toggle_scan()
            self.worker.stop()
            self.worker.wait(2000)
            self.worker = None
            self.connect_btn.setText("Connect")
            self.connect_btn.setProperty("connected", "false")
            self.connect_btn.setStyle(self.connect_btn.style())
            self.scan_btn.setEnabled(False)
            self._on_port_status("Not connected")
            self._update_all_action_buttons()

    def _on_port_status(self, text: str):
        self.sidebar_subtitle.setText(text)
        connected = self.worker is not None
        dot = "●" if connected else "○"
        color = COLOR_SECONDARY if connected else COLOR_TEXT_DIM
        self.footer_status.setText(f"{dot} {text}")
        self.footer_status.setStyleSheet(f"color: {color};")

    def _on_raw_line(self, line: str):
        self.terminal.appendPlainText(line)

    def _toggle_scan(self):
        if not self.worker:
            return
        if self._scanning:
            self.worker.send_command("STOP")
            self.scan_btn.setText(f"{ICON_SCAN}  Start Scan")
            self._scanning = False
            self.status_pill.setText("STATUS: IDLE")
            self.status_pill.setStyleSheet(_state_pill_style("UNKNOWN"))
        else:
            self.worker.send_command("SCAN")
            self.scan_btn.setText(f"{ICON_SCAN}  Stop Scan")
            self._scanning = True
            self.status_pill.setText("STATUS: SCANNING")
            self.status_pill.setStyleSheet(_state_pill_style("INITED"))

    # ------------------------------------------------------------------
    # EVT handlers
    # ------------------------------------------------------------------

    def _get_or_create(self, node_id: int) -> NodeInfo:
        if node_id not in self.nodes:
            self.nodes[node_id] = NodeInfo(node_id=node_id)
        return self.nodes[node_id]

    def _on_node_seen(self, node_id: int, rssi: int, fw_state: str):
        is_new = node_id not in self.nodes
        info = self._get_or_create(node_id)
        info.rssi = rssi
        info.fw_state = fw_state
        info.last_seen = time.time()
        if is_new:
            self._refresh_table()
        else:
            self._update_row_live(node_id)

    def _on_node_connecting(self, node_id: int):
        info = self._get_or_create(node_id)
        info.connection_state = "connecting"
        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_connecting()
        self._update_all_action_buttons()

    def _on_node_connected(self, node_id: int):
        info = self._get_or_create(node_id)
        info.connection_state = "connected"
        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_connected()
        self._update_all_action_buttons()

    def _on_node_disconnected(self, node_id: int):
        info = self._get_or_create(node_id)
        if info.connection_state != "idle":
            info.connection_state = "idle"
            if node_id in self._status_widgets:
                self._status_widgets[node_id].set_idle()
        self._update_all_action_buttons()

    def _on_init_ok(self, node_id: int):
        info = self._get_or_create(node_id)
        info.fw_state = "INITED"
        info.connection_state = "idle"
        self._refresh_table()

    def _on_download_header_seen(self, node_id: int, total: int):
        info = self._get_or_create(node_id)
        info.download_total = total
        info.connection_state = "downloading"
        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_downloading(info.download_count, total)

    def _on_download_progress(self, node_id: int, count: int, total: int):
        info = self._get_or_create(node_id)
        info.download_count = count
        if total > 0:
            info.download_total = total
        info.connection_state = "downloading"

        # Update in place -- NO table teardown/rebuild, completely smooth!
        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_downloading(count, info.download_total)
        else:
            self._refresh_table()

    def _on_download_reading(self, node_id: int, seq: int, millis_since_init: int,
                              temp_cdeg: int, humidity_pct10: int):
        node_manager_db.insert_reading(
            self.db_conn, node_id, seq, millis_since_init, temp_cdeg, humidity_pct10,
            downloaded_at=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        )

    def _on_download_ok(self, node_id: int, entries: int):
        self.db_conn.commit()
        info = self._get_or_create(node_id)
        info.fw_state = "DOWNLOADED"
        info.connection_state = "idle"
        info.download_count = entries
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        info.last_downloaded_at = ts
        info.last_entries_count = entries
        node_manager_db.record_download(self.db_conn, node_id, ts, entries)

        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_synced(entries)

        self._refresh_table()

    def _on_node_error(self, node_id: int, msg: str):
        self.db_conn.commit()
        info = self._get_or_create(node_id)
        info.connection_state = "idle"
        self._on_port_status(f"Node {node_id}: {msg}")
        if node_id in self._status_widgets:
            self._status_widgets[node_id].set_idle()
        self._refresh_table()

    # ------------------------------------------------------------------
    # Table rendering
    # ------------------------------------------------------------------

    def _check_stale_nodes(self):
        now = time.time()
        node_ids = sorted(self.nodes.keys())
        for row, node_id in enumerate(node_ids):
            info = self.nodes[node_id]
            stale = info.last_seen > 0 and (now - info.last_seen) > STALE_NODE_TIMEOUT_S
            item = self.table.item(row, COL_RSSI)
            if item:
                item.setForeground(QColor(COLOR_TEXT_DIM if stale else COLOR_SECONDARY))

    def _update_row_live(self, node_id: int):
        node_ids = sorted(self.nodes.keys())
        if node_id not in node_ids:
            self._refresh_table()
            return
        row = node_ids.index(node_id)
        info = self.nodes[node_id]
        rssi_text = f"{info.rssi} dBm" if info.rssi is not None else "-"
        rssi_item = self.table.item(row, COL_RSSI)
        if rssi_item:
            rssi_item.setText(rssi_text)
            rssi_item.setForeground(QColor(COLOR_SECONDARY))

    def _update_all_action_buttons(self):
        is_connected = self.worker is not None
        any_busy = any(n.connection_state != "idle" for n in self.nodes.values())
        for node_id, action_widget in self._action_widgets.items():
            info = self.nodes.get(node_id)
            node_busy = not is_connected or any_busy or (info is not None and info.connection_state != "idle")
            has_data = info is not None and ((info.last_entries_count or 0) > 0 or info.fw_state == "DOWNLOADED")
            action_widget.update_states(node_busy, has_data)

    def _refresh_table(self):
        node_ids = sorted(self.nodes.keys())
        self.table.setRowCount(len(node_ids))

        now = time.time()
        active_count = 0
        is_connected = self.worker is not None
        any_busy = any(n.connection_state != "idle" for n in self.nodes.values())

        for row, node_id in enumerate(node_ids):
            info = self.nodes[node_id]
            stale = info.last_seen > 0 and (now - info.last_seen) > STALE_NODE_TIMEOUT_S
            if info.connection_state != "idle":
                active_count += 1

            id_item = QTableWidgetItem(f"Node #{node_id:02d}")
            id_item.setFont(QFont("Consolas", 10, QFont.Bold))
            id_item.setTextAlignment(Qt.AlignCenter)
            self.table.setItem(row, COL_NODE_ID, id_item)

            rssi_text = f"{info.rssi} dBm" if info.rssi is not None else "-"
            rssi_item = QTableWidgetItem(rssi_text)
            rssi_item.setTextAlignment(Qt.AlignCenter)
            rssi_item.setForeground(QColor(COLOR_SECONDARY if not stale else COLOR_TEXT_DIM))
            self.table.setItem(row, COL_RSSI, rssi_item)

            self.table.setCellWidget(row, COL_FW_STATE, self._make_state_pill(info.fw_state))

            # Re-use or create status widget
            if node_id not in self._status_widgets:
                self._status_widgets[node_id] = StatusCellWidget()
            status_w = self._status_widgets[node_id]
            if info.connection_state == "downloading":
                status_w.set_downloading(info.download_count, info.download_total)
            elif info.connection_state == "connecting":
                status_w.set_connecting()
            elif info.connection_state == "connected":
                status_w.set_connected()
            elif info.fw_state == "DOWNLOADED" and info.last_entries_count:
                status_w.set_synced(info.last_entries_count)
            else:
                status_w.set_idle()
            self.table.setCellWidget(row, COL_CONN_STATE, status_w)

            last_dl_item = QTableWidgetItem(info.last_downloaded_at or "Never")
            last_dl_item.setTextAlignment(Qt.AlignCenter)
            self.table.setItem(row, COL_LAST_DOWNLOAD, last_dl_item)

            entries_text = str(info.last_entries_count) if info.last_entries_count is not None else "-"
            entries_item = QTableWidgetItem(entries_text)
            entries_item.setTextAlignment(Qt.AlignCenter)
            entries_item.setFont(QFont("Consolas", 10))
            self.table.setItem(row, COL_ENTRIES, entries_item)

            # Re-use or create action widget
            if node_id not in self._action_widgets:
                self._action_widgets[node_id] = ActionCellWidget(
                    node_id,
                    on_init=lambda n: self._send_targeted(n, "INIT"),
                    on_download=lambda n: self._download_clicked(n),
                    on_graph=lambda n: self._show_graph(n),
                    on_export=lambda n: self._export_clicked(n),
                )
            action_w = self._action_widgets[node_id]
            node_busy = not is_connected or any_busy or info.connection_state != "idle"
            has_data = (info.last_entries_count or 0) > 0 or info.fw_state == "DOWNLOADED"
            action_w.update_states(node_busy, has_data)
            self.table.setCellWidget(row, COL_ACTIONS, action_w)

        self.stat_total_value.setText(str(len(node_ids)))
        self.stat_active_value.setText(str(active_count))
        latest = None
        for info in self.nodes.values():
            if info.last_downloaded_at and (latest is None or info.last_downloaded_at > latest[0]):
                latest = (info.last_downloaded_at, info.node_id)
        self.stat_latest_value.setText(f"Node #{latest[1]:02d}" if latest else "--")

    def _make_state_pill(self, state: str) -> QWidget:
        wrap = QWidget()
        lay = QHBoxLayout(wrap)
        lay.setContentsMargins(4, 4, 4, 4)
        lay.setAlignment(Qt.AlignCenter)
        pill = QLabel(state)
        pill.setStyleSheet(_state_pill_style(state))
        pill.setAlignment(Qt.AlignCenter)
        lay.addWidget(pill)
        return wrap

    def _send_targeted(self, node_id: int, verb: str):
        if not self.worker:
            return
        self.worker.send_command(f"{verb} {node_id}")

    def _download_clicked(self, node_id: int):
        fw_state = self.nodes[node_id].fw_state
        if fw_state == "UNKNOWN":
            reply = QMessageBox.question(
                self, "Node not initialized this session",
                f"Node {node_id} hasn't been initialized (INIT) this session (state: {fw_state}).\n"
                "Downloading anyway may return stale data left over from a previous "
                "experiment or bench test.\n\nDownload anyway?",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No,
            )
            if reply != QMessageBox.Yes:
                return
        self._send_targeted(node_id, "DOWNLOAD")

    def _show_graph(self, node_id: int):
        dialog = DownloadChartDialog(self.db_conn, node_id, self)
        dialog.exec_()

    def _export_clicked(self, node_id: int):
        rows = node_manager_db.query_readings(self.db_conn, node_id)
        if not rows:
            QMessageBox.information(self, "No data",
                                     f"No downloaded data stored for node {node_id} yet.")
            return

        folder = QFileDialog.getExistingDirectory(self, f"Choose folder to save node {node_id}'s CSV")
        if not folder:
            return

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        out_path = Path(folder) / f"node{node_id}_{ts}.csv"
        try:
            count = node_manager_db.export_csv(self.db_conn, node_id, out_path)
        except OSError as e:
            QMessageBox.critical(self, "Export failed", f"Could not write {out_path}:\n{e}")
            return

        QMessageBox.information(self, "Export complete",
                                 f"Wrote {count} rows to {out_path}")

    def closeEvent(self, event):
        if self.worker:
            self.worker.stop()
            self.worker.wait(2000)
        event.accept()


def main():
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
