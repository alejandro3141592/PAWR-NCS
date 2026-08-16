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
    last_epoch          INTEGER high-water mark from that download's EVT
                                DOWNLOAD_OK ... EPOCH ... MS ... line (added
                                2026-08-14 for incremental downloads -- see
                                common/pawr_protocol.h's download_req/
                                download_done comments). Sent back as the
                                since_epoch of the NEXT download so central/
                                the peripheral only send what's new. 0 if
                                never downloaded or the connecting central
                                predates this field (see CentralSerialWorker's
                                fallback parsing).
    last_ms              INTEGER high-water mark's millis_since_init,
                                paired with last_epoch (see above -- only
                                meaningful together, an epoch without its ms
                                or vice versa doesn't identify a point in
                                time).

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
                                 x-axis node_manager_gui.py's chart plots --
                                 but ONLY within a single init_epoch, see below.
    init_epoch          INTEGER which boot/init cycle this reading belongs to
                                 (added 2026-08-14 for incremental downloads,
                                 parsed from EVT DOWNLOAD_DATA's EPOCH field --
                                 see common/pawr_protocol.h's sensor_payload
                                 comment). millis_since_init resets to a new
                                 baseline on every reboot+reinit, so two
                                 readings with different init_epoch are NOT
                                 on the same timeline even if their ms values
                                 overlap -- confirmed on real hardware
                                 2026-08-14: query_wall_clock_anchor() and the
                                 chart both ignored epoch before this field
                                 existed, and picked/plotted rows spanning two
                                 different boots, showing wall-clock times
                                 ~15 minutes off. query_readings() and the
                                 chart now only use the node's MOST RECENT
                                 epoch, never mixing epochs on one axis.
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
         match across two different boots' t0 references. init_epoch (see
         above) makes this explicit rather than merely probable.
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
    # CREATE TABLE IF NOT EXISTS above doesn't add columns to a
    # node_manager.db left over from before 2026-08-14 (incremental
    # downloads) -- migrate in place rather than requiring a fresh DB file.
    existing_cols = {row[1] for row in conn.execute("PRAGMA table_info(node_downloads)")}
    if "last_epoch" not in existing_cols:
        conn.execute("ALTER TABLE node_downloads ADD COLUMN last_epoch INTEGER NOT NULL DEFAULT 0")
    if "last_ms" not in existing_cols:
        conn.execute("ALTER TABLE node_downloads ADD COLUMN last_ms INTEGER NOT NULL DEFAULT 0")
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
    readings_cols = {row[1] for row in conn.execute("PRAGMA table_info(readings)")}
    if "init_epoch" not in readings_cols:
        # Existing rows predate epoch tracking -- 0 is the only sane
        # default (matches a first-ever/never-rebooted node's epoch), and
        # any of these older multi-epoch-contaminated rows get superseded
        # on the next real download anyway (see query_readings()/
        # query_wall_clock_anchor(), both now epoch-aware).
        conn.execute("ALTER TABLE readings ADD COLUMN init_epoch INTEGER NOT NULL DEFAULT 0")
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_readings_node_ms ON readings(node_id, millis_since_init)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_readings_node_epoch ON readings(node_id, init_epoch)"
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
    init_epoch: int = 0,
) -> None:
    """init_epoch defaults to 0 only for callers with nothing better (e.g. a
    central predating EVT DOWNLOAD_DATA's EPOCH field, see
    CentralSerialWorker._handle_line's fallback) -- a real download always
    has a real value, parsed from that line.

    downloaded_at is deliberately NOT overwritten on a re-download of the
    same (node_id, seq, millis_since_init) -- it means "the first time this
    GUI ever learned this reading existed," which is what
    query_wall_clock_anchor() needs to approximate real time. Confirmed on
    real hardware 2026-08-14: refreshing it on every re-download made the
    anchor's implied "epoch started at" timestamp slide forward by however
    long ago the epoch actually started (since the low-ms anchor row's
    downloaded_at kept getting bumped to "now" on each re-download, even
    though its content -- and the real moment it happened -- never
    changed), producing wall-clock times far in the future. temp_cdeg/
    humidity_pct10/init_epoch still update normally -- only downloaded_at is
    frozen at first-seen.
    """
    conn.execute(
        "INSERT INTO readings (node_id, seq, millis_since_init, temp_cdeg, humidity_pct10, downloaded_at, init_epoch)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(node_id, seq, millis_since_init) DO UPDATE SET"
        "   temp_cdeg = excluded.temp_cdeg,"
        "   humidity_pct10 = excluded.humidity_pct10,"
        "   init_epoch = excluded.init_epoch",
        (node_id, seq, millis_since_init, temp_cdeg, humidity_pct10, downloaded_at, init_epoch),
    )
    # Not committed here -- callers insert many rows per download and should
    # commit once at the end (see node_manager_gui.py's _on_download_ok),
    # not once per row.


def query_latest_epoch(conn: sqlite3.Connection, node_id: int) -> int:
    """Highest init_epoch stored for node_id, or 0 if nothing stored yet.
    Readings from any OLDER epoch are a different, incomparable timeline
    (see this file's header) and must never be mixed with the latest
    epoch's on the same chart/anchor -- this is the single source of truth
    both query_readings() and query_wall_clock_anchor() use to decide what
    counts as "current."""
    cur = conn.execute(
        "SELECT MAX(init_epoch) FROM readings WHERE node_id = ?", (node_id,)
    )
    row = cur.fetchone()
    return row[0] if row and row[0] is not None else 0


def query_readings(conn: sqlite3.Connection, node_id: int) -> List[Tuple]:
    """Return every stored reading for node_id FROM ITS MOST RECENT
    init_epoch only, ordered by millis_since_init: (seq, millis_since_init,
    temp_cdeg, humidity_pct10). Older epochs are excluded -- see
    query_latest_epoch()'s docstring for why mixing them is wrong, not just
    stale. Confirmed necessary on real hardware 2026-08-14: without this,
    the chart/CSV export mixed two different boots' timelines on one axis."""
    latest_epoch = query_latest_epoch(conn, node_id)
    cur = conn.execute(
        "SELECT seq, millis_since_init, temp_cdeg, humidity_pct10 FROM readings"
        " WHERE node_id = ? AND init_epoch = ? ORDER BY millis_since_init",
        (node_id, latest_epoch),
    )
    return cur.fetchall()


def query_wall_clock_anchor(conn: sqlite3.Connection, node_id: int) -> Optional[Tuple[str, int]]:
    """Returns (downloaded_at, millis_since_init) for the row with the
    smallest millis_since_init in this node's MOST RECENT init_epoch only --
    i.e. the reading closest to the CURRENT epoch's t=0, from whichever
    download first captured it.

    Restricting to the latest epoch (see query_latest_epoch()) matters:
    confirmed on real hardware 2026-08-14 that without it, this could anchor
    to a smallest-ms row from an OLDER epoch (a previous boot's timeline),
    throwing off every computed wall-clock time by however long passed
    between that old epoch and the current one (~15 minutes in the case that
    exposed this).

    Nodes have no RTC (see this file's header), so there is no true
    wall-clock timestamp per reading -- only downloaded_at, which is when
    THIS GUI received the row, not when the node measured it. Approximating
    "real time" for a reading means anchoring to this row: every other
    reading's implied wall-clock time is
        anchor_downloaded_at - (anchor_ms - reading_ms) / 1000 seconds.
    This is only as accurate as how soon after init the first download
    happened to run -- if the node measured for an hour before anyone
    downloaded it, every timestamp is off by about that hour. There is no
    way to do better without giving nodes an RTC, which none of the boards
    in this project have.
    """
    latest_epoch = query_latest_epoch(conn, node_id)
    cur = conn.execute(
        "SELECT downloaded_at, millis_since_init FROM readings"
        " WHERE node_id = ? AND init_epoch = ? ORDER BY millis_since_init ASC LIMIT 1",
        (node_id, latest_epoch),
    )
    row = cur.fetchone()
    return (row[0], row[1]) if row else None


def export_csv(conn: sqlite3.Connection, node_id: int, out_path: Path) -> int:
    """Writes node_id's stored readings to out_path as CSV (seq, millis_since_init,
    temp_c, humidity_pct -- converted from the raw wire units query_readings()
    returns, since a hand-off CSV should be human-readable, not need the same
    /100 and /10 the chart code applies internally). Only the node's MOST
    RECENT init_epoch is exported (see query_readings()) -- same reasoning as
    the chart, older epochs are a different boot's timeline. Returns the row
    count written; raises OSError if out_path can't be created (permissions,
    a directory that no longer exists, etc.) -- caller's responsibility to
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


def record_download(
    conn: sqlite3.Connection,
    node_id: int,
    ts: str,
    entries_count: int,
    last_epoch: int = 0,
    last_ms: int = 0,
) -> None:
    """last_epoch/last_ms default to 0 (not "leave unchanged") only for
    callers that genuinely have nothing better -- a real download always
    has real values from its EVT DOWNLOAD_OK ... EPOCH ... MS ... line (see
    node_manager_gui.py's _on_download_ok), including 0/0 itself when
    nothing new was found (the peripheral echoes the requested high-water
    mark back unchanged in that case, see peripheral/src/main.c)."""
    conn.execute(
        "INSERT INTO node_downloads (node_id, last_downloaded_at, entries_count, last_epoch, last_ms)"
        " VALUES (?, ?, ?, ?, ?)"
        " ON CONFLICT(node_id) DO UPDATE SET"
        "   last_downloaded_at = excluded.last_downloaded_at,"
        "   entries_count = excluded.entries_count,"
        "   last_epoch = excluded.last_epoch,"
        "   last_ms = excluded.last_ms",
        (node_id, ts, entries_count, last_epoch, last_ms),
    )
    conn.commit()


def load_all(conn: sqlite3.Connection) -> Dict[int, Dict]:
    """Returns {node_id: {"last_downloaded_at": str, "entries_count": int,
    "last_epoch": int, "last_ms": int}}."""
    cur = conn.execute(
        "SELECT node_id, last_downloaded_at, entries_count, last_epoch, last_ms FROM node_downloads"
    )
    return {
        row[0]: {
            "last_downloaded_at": row[1],
            "entries_count": row[2],
            "last_epoch": row[3],
            "last_ms": row[4],
        }
        for row in cur.fetchall()
    }
