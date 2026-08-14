"""
node_manager_db.py -- Shared SQLite helper for node_manager_gui.py.

DB file: <project_root>/gui/node_manager.db

Table: node_downloads
    node_id             INTEGER PRIMARY KEY
    last_downloaded_at  TEXT    ISO-ish datetime "YYYY-MM-DD HH:MM:SS", wall-clock
                                (central/nodes have no RTC -- this timestamp is only
                                ever meaningful on the PC that ran the download, see
                                node_manager_gui.py's file header)
    entries_count       INTEGER entries received in that last download

Table: readings
    id                  INTEGER  PK AUTOINCREMENT
    node_id             INTEGER
    seq                 INTEGER  peripheral-local rolling counter
    millis_since_init   INTEGER  ms since that node's own init-phase t0 -- the
                                 only timeline downloaded data actually has
                                 (see common/pawr_protocol.h's file header for
                                 why there's no wall-clock timestamp per
                                 reading: no board in this project has an
                                 RTC, and these readings were taken with no
                                 live connection to stamp them against
                                 anyway). This, not wall-clock time, is the
                                 x-axis node_manager_gui.py's chart plots.
    temp_cdeg           INTEGER  raw wire value, hundredths of a degree C
    humidity_pct10      INTEGER  raw wire value, tenths of a percent
    downloaded_at       TEXT     wall-clock time this GUI received the row
                                 (for dedup/audit only, not for plotting)

    A given (node_id, seq) can appear more than once for two different
    reasons, so the uniqueness/upsert key is (node_id, seq,
    millis_since_init), not just (node_id, seq):
      1. Repeated downloads of the same node (central always re-sends the
         whole flash log) legitimately re-send the exact same reading --
         same seq AND same millis_since_init. This should upsert in place,
         not duplicate.
      2. seq is a plain rolling counter that resets to 0 on every node
         reboot (see peripheral/src/main.c's s_seq), but the flash log
         persists across reboots -- so two DIFFERENT readings from two
         different boots can share the same seq. Confirmed on real
         hardware 2026-08-14: a single download of one long-running,
         once-rebooted node returned 900 raw entries but only 580 unique
         (node_id, seq) pairs, because seq wrapped back through
         0..N twice. Keying only on (node_id, seq) silently discarded the
         earlier boot's readings on upsert -- real data loss, not a
         cosmetic duplicate. millis_since_init distinguishes them because
         it's derived from k_uptime_get() at t0, which is set at
         write_start_measuring() time and (barring a real-time-clock,
         which no board here has) is extremely unlikely to coincidentally
         match across two different boots' t0 references.
"""

import csv
import sqlite3
from pathlib import Path
from typing import Dict, List, Optional, Tuple

DB_PATH = Path(__file__).resolve().parent / "node_manager.db"


def open_db() -> sqlite3.Connection:
    """Open (or create) the DB, enable WAL mode, ensure schema exists."""
    conn = sqlite3.connect(str(DB_PATH), check_same_thread=False, timeout=10)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS node_downloads (
            node_id            INTEGER PRIMARY KEY,
            last_downloaded_at TEXT    NOT NULL,
            entries_count      INTEGER NOT NULL
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id                INTEGER PRIMARY KEY AUTOINCREMENT,
            node_id           INTEGER NOT NULL,
            seq               INTEGER NOT NULL,
            millis_since_init INTEGER NOT NULL,
            temp_cdeg         INTEGER,
            humidity_pct10    INTEGER,
            downloaded_at     TEXT    NOT NULL,
            UNIQUE(node_id, seq, millis_since_init)
        )
    """)
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_readings_node_ms ON readings(node_id, millis_since_init)"
    )
    conn.commit()
    return conn


def insert_reading(
    conn: sqlite3.Connection,
    node_id: int,
    seq: int,
    millis_since_init: int,
    temp_cdeg: Optional[int],
    humidity_pct10: Optional[int],
    downloaded_at: str,
) -> None:
    conn.execute(
        "INSERT INTO readings (node_id, seq, millis_since_init, temp_cdeg, humidity_pct10, downloaded_at)"
        " VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(node_id, seq, millis_since_init) DO UPDATE SET"
        "   temp_cdeg = excluded.temp_cdeg,"
        "   humidity_pct10 = excluded.humidity_pct10,"
        "   downloaded_at = excluded.downloaded_at",
        (node_id, seq, millis_since_init, temp_cdeg, humidity_pct10, downloaded_at),
    )
    # Not committed here -- callers insert many rows per download and should
    # commit once at the end (see node_manager_gui.py's _on_download_ok),
    # not once per row.


def query_readings(conn: sqlite3.Connection, node_id: int) -> List[Tuple]:
    """Return every stored reading for node_id, ordered by millis_since_init:
    (seq, millis_since_init, temp_cdeg, humidity_pct10)."""
    cur = conn.execute(
        "SELECT seq, millis_since_init, temp_cdeg, humidity_pct10 FROM readings"
        " WHERE node_id = ? ORDER BY millis_since_init",
        (node_id,),
    )
    return cur.fetchall()


def export_csv(conn: sqlite3.Connection, node_id: int, out_path: Path) -> int:
    """Writes node_id's stored readings to out_path as CSV (seq, millis_since_init,
    temp_c, humidity_pct -- converted from the raw wire units query_readings()
    returns, since a hand-off CSV should be human-readable, not need the same
    /100 and /10 the chart code applies internally). Returns the row count
    written; raises OSError if out_path can't be created (permissions, a
    directory that no longer exists, etc.) -- caller's responsibility to
    report that to the operator."""
    rows = query_readings(conn, node_id)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["seq", "millis_since_init", "temp_c", "humidity_pct"])
        for seq, ms, temp_cdeg, humidity_pct10 in rows:
            writer.writerow([
                seq,
                ms,
                f"{temp_cdeg / 100:.2f}" if temp_cdeg is not None else "",
                f"{humidity_pct10 / 10:.1f}" if humidity_pct10 is not None else "",
            ])
    return len(rows)


def record_download(conn: sqlite3.Connection, node_id: int, ts: str, entries_count: int) -> None:
    conn.execute(
        "INSERT INTO node_downloads (node_id, last_downloaded_at, entries_count)"
        " VALUES (?, ?, ?)"
        " ON CONFLICT(node_id) DO UPDATE SET"
        "   last_downloaded_at = excluded.last_downloaded_at,"
        "   entries_count = excluded.entries_count",
        (node_id, ts, entries_count),
    )
    conn.commit()


def load_all(conn: sqlite3.Connection) -> Dict[int, Dict]:
    """Returns {node_id: {"last_downloaded_at": str, "entries_count": int}}."""
    cur = conn.execute("SELECT node_id, last_downloaded_at, entries_count FROM node_downloads")
    return {
        row[0]: {"last_downloaded_at": row[1], "entries_count": row[2]}
        for row in cur.fetchall()
    }
