"""
sensor_db.py -- Shared SQLite helper for sensor_gui.py.

DB file: <project_root>/gui/sensor_data.db

Table: readings
    id          INTEGER  PK AUTOINCREMENT
    ts          TEXT     ISO-ish datetime "YYYY-MM-DD HH:MM:SS.mmm"
    node_id     INTEGER
    seq         INTEGER  peripheral-local rolling counter (see
                         common/pawr_protocol.h struct sensor_payload)
    flags       INTEGER  bit0: temp invalid, bit1: humidity invalid
    temperature REAL     skin temperature, degrees C (nullable)
    humidity    REAL     relative humidity, % (nullable)
"""

import sqlite3
from datetime import datetime, timedelta
from pathlib import Path
from typing import List, Optional, Tuple

DB_PATH = Path(__file__).resolve().parent / "sensor_data.db"


def open_db() -> sqlite3.Connection:
    """Open (or create) the DB, enable WAL mode, ensure schema exists."""
    conn = sqlite3.connect(str(DB_PATH), check_same_thread=False, timeout=10)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          TEXT    NOT NULL,
            node_id     INTEGER NOT NULL,
            seq         INTEGER,
            flags       INTEGER,
            temperature REAL,
            humidity    REAL
        )
    """)
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_node_ts ON readings(node_id, ts)"
    )
    conn.commit()
    return conn


def insert_reading(
    conn: sqlite3.Connection,
    ts: str,
    node_id: int,
    seq: Optional[int],
    flags: Optional[int],
    temperature: Optional[float],
    humidity: Optional[float],
) -> None:
    conn.execute(
        "INSERT INTO readings (ts, node_id, seq, flags, temperature, humidity)"
        " VALUES (?, ?, ?, ?, ?, ?)",
        (ts, node_id, seq, flags, temperature, humidity),
    )
    conn.commit()


def query_history(
    conn: sqlite3.Connection,
    node_id: int,
    hours: float = 1.0,
) -> List[Tuple]:
    """Return rows: (ts, temperature, humidity)."""
    since = (datetime.now() - timedelta(hours=hours)).strftime("%Y-%m-%d %H:%M:%S")
    cur = conn.execute(
        "SELECT ts, temperature, humidity FROM readings"
        " WHERE node_id = ? AND ts >= ?"
        " ORDER BY ts",
        (node_id, since),
    )
    return cur.fetchall()


def query_all_for_export(conn: sqlite3.Connection) -> List[Tuple]:
    return conn.execute(
        "SELECT ts, node_id, seq, flags, temperature, humidity"
        " FROM readings ORDER BY ts"
    ).fetchall()
