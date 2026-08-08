# Packet delivery rate vs. distance — node 24, central ID 1

**Date:** 2026-08-08
**Setup:** `NUM_SUBEVENTS=17` (see `distance-test-17slot` branch), 17-slot table
with only node 24 physically powered (16 unpowered placeholder slots keep
central's real polling load representative of the full 17-node deployment).
`PAWR_INTERVAL_MS` = 10.00s, so each expected delivery window is
`floor(recording_seconds / 10)` packets.

**Method:** Central's serial console logs one line per response actually
received (`>>> Node 24 (subevent 16): ... seq=N`). The peripheral's `seq`
counter free-runs once per 10s interval regardless of whether the response
reaches central, so gaps in the `seq` sequence observed at central directly
count missed round-trips. Delivery rate = received / (max_seq - min_seq + 1)
over each 10-minute window. Raw captures: `distance_test_<point>.log`
(timestamped by `tools/Watch-SerialLog.ps1`).

## Results

| Distance | seq range | Expected | Received | Missing | Delivery rate |
|----------|-----------|----------|----------|---------|----------------|
| Close (~0m) | 44–105   | 62 | 62 | 0  | **100.0%** |
| 0.5 m       | 106–172  | 67 | 64 | 3  | **95.5%**  |
| 1 m         | 173–246  | 74 | 61 | 13 | **82.4%**  |
| 1.5 m       | 250–318  | 69 | 49 | 20 | **71.0%**  |
| 2 m         | 368–427  | 60 | 47 | 13 | **78.3%**  |

## Notes / caveats

- **1.5 m**: capture began with a fresh reconnect + PAST resync
  (`Found peripheral ... Connected ... PAST sent`) rather than mid-stream,
  i.e. sync had dropped and re-established just before this window started.
  Left in as-is since a real reconnect-driven gap is a legitimate part of
  reliability at range, not a measurement artifact.
- **2 m**: the raw log (`distance_test_2m.log`) starts with a burst of
  seq 319–331 all logged within the same millisecond, then jumps straight to
  seq 368 — this is the USB-CDC console ring buffer overflowing during the
  idle gap between recordings (board kept receiving/responding over BLE the
  whole time; only the *console print* backlog was lost when nothing was
  reading the port), the same failure mode documented in NOTES.md
  2026-08-07 for the node 49 flash-dump retrieval. **Seq 332–367 are
  unrecoverable console drops, not real RF packet loss**, so the 2 m
  delivery rate above is computed only from the clean tail (seq 368–427,
  ~590s) to keep it comparable to the other points. The full raw log is
  still kept for reference.
- Delivery rate does **not** decrease perfectly monotonically (1.5 m at
  71.0% is lower than 2 m at 78.3%) — plausible given the small per-point
  sample size (~60-70 packets) and real-world multipath/orientation
  variance rather than a measurement error; worth flagging as a limitation
  if used in the thesis writeup, and a candidate for a repeat run with
  longer per-point windows if time allows.
- `NUM_SUBEVENTS` must be reverted to 25 (or whatever the live roster needs)
  before any CENTRAL_ID=2 board is rebuilt/reflashed — see NOTES.md
  2026-08-08 on the `distance-test-17slot` branch.
