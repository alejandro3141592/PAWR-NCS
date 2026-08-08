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

## Results — baseline (0 dBm TX power, 1M PHY)

| Distance | seq range | Expected | Received | Missing | Delivery rate |
|----------|-----------|----------|----------|---------|----------------|
| Close (~0m) | 44–105   | 62 | 62 | 0  | **100.0%** |
| 0.5 m       | 106–172  | 67 | 64 | 3  | **95.5%**  |
| 1 m         | 173–246  | 74 | 61 | 13 | **82.4%**  |
| 1.5 m       | 250–318  | 69 | 49 | 20 | **71.0%**  |
| 2 m         | 368–427  | 60 | 47 | 13 | **78.3%**  |

## Results — improvement experiments

nRF52840 has no external front-end amp, so **+8 dBm is the hardware ceiling**
for TX power on this board (confirmed against `zephyr/subsys/bluetooth/
controller/Kconfig`: every level above +8 dBm is gated to `SOC_SERIES_NRF54H`
or ESP32, not this SoC). `CONFIG_BT_CTLR_TX_PWR_PLUS_8=y` was added to both
`central/prj.conf` and `peripheral/prj.conf`. LE Coded PHY (S=8) was tested on
top of that by swapping central's advertising set to `BT_LE_EXT_ADV_CODED_NCONN`
(`central/src/main.c`, `bt_le_ext_adv_create` call) plus
`CONFIG_BT_CTLR_PHY_CODED=y` on both apps — periodic sync PHY is
auto-detected from the advertising PDU, so only central needed rebuilding/
reflashing to toggle Coded PHY on and off; peripheral was flashed once and
left alone for the rest of these runs.

| Distance | 0 dBm (baseline) | +8 dBm | +8 dBm + Coded PHY |
|----------|-------------------|--------|---------------------|
| 1 m   | 82.4% | **96.9%** (62/64) | 96.7% (58/60) |
| 1.5 m | 71.0% | **91.9%** (57/62) | **93.4%** (57/61) |
| 2 m   | 78.3% | 89.0% (65/73)     | **94.0%** (63/67) |

**Takeaways:**
- TX power does almost all of the work here. Going from 0 to +8 dBm alone
  recovered most of the range-related loss at every distance tested.
- Coded PHY's marginal benefit over +8dBm-only grows with distance: within
  noise at 1m (96.9% vs 96.7%, ~60-packet samples), a small +1.5pp at 1.5m,
  a clearer +5pp at 2m. Consistent with FEC mattering more as the link
  margin shrinks.
- `PAWR_RESPONSE_SLOT_SPACING` (10ms) turned out to be sufficient for Coded
  PHY responses without any timing rework — no systematic slot-timing
  failures observed in either Coded PHY run, just isolated single-packet
  misses in line with the baseline runs.
- The first 1.5m +8dBm-only attempt (not included above) got 0 packets in a
  full 10-minute window — central had just been reflashed/rebooted and
  never re-onboarded node 24 during that window. Cause wasn't conclusively
  diagnosed (peripheral position/power at the time is unconfirmed); a
  straight retry succeeded cleanly. Worth planning for a brief "re-onboard
  at close range" step after any central reflash in future runs, since a
  central reboot invalidates the peripheral's existing connection/sync.

## Notes / caveats (baseline sweep)

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
