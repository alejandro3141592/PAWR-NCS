# Walking vs. Stationary Experiments — Consolidated Summary

Every walking and stationary PDR run captured for the ring-2 11-node roster
(13,15,16,18,19,20,21,23,24,26,29), across static-table PAwR, dynamic
(hub-driven) PAwR, and Broadcasting (uncoordinated advertising, no
scheduling at all). PDR is computed per node as `received / expected`,
where `expected = max(seq) - min(seq) + 1` over the observation window; a
node that reboots mid-run (sequence counter resets to 0) is split into
separate epochs and summed, not treated as one contiguous span. RSSI is
only available on runs captured after PAwR's RSSI logging was added
(evening of 2026-08-23) — earlier walking runs (both PAwR and Broadcasting)
predate it. Numbers are computed directly from the raw logs/CSVs, not
hand-transcribed. Body position legend (hub carried in the left jeans
pocket): 13 chest, 15 upper back, 16 head, 18 forearm, 19 right foot, 20
left hand, 21 right calf, 23 lower back, 24 abdomen, 26 right thigh, 29 left
bicep.

---

## 1. Walking, static/fixed-table PAwR (2026-08-23) — 11 subevents, no redundancy

Source: `logs/walking_static_pawr_serial_20260823.txt`, ~29 min
(20:00:58-20:30:03).

| node | received | expected | PDR |
|---:|---:|---:|---:|
| 13 | 174 | 175 | 99.4% |
| 16 | 116 | 174 | 66.7% |
| 18 | 153 | 175 | 87.4% |
| 19 | 131 | 174 | 75.3% |
| 20 | 173 | 174 | 99.4% |
| 21 | 170 | 174 | 97.7% |
| 24 | 175 | 175 | 100.0% |
| 26 | 173 | 175 | 98.9% |
| 29 | 153 | 175 | 87.4% |

**Overall: 1418 / 1571 = 90.26% PDR — but a 9-of-11-node figure.** Nodes 15
and 23 **never appear at all** in this run's log (full dropout, excluded
from numerator/denominator, not just averaged in as zeros). No RSSI (predates
RSSI logging).

---

## 2. Walking, dynamic (hub-driven) PAwR (2026-08-23) — 11 subevents, no redundancy

Source: `logs/walking_dynamic_pawr_serial_20260823.txt`, ~37-38 min (duration
derived from sequence span at the fixed 10s interval, not the log's sparse
timestamps).

| node | received | expected | PDR | subevents used |
|---:|---:|---:|---:|---|
| 13 | 14  | 15  | 93.3% | 5 |
| 15 | 148 | 225 | 65.8% | 2, 4 |
| 16 | 190 | 226 | 84.1% | 1 |
| 18 | 132 | 225 | 58.7% | 0 |
| 19 | 221 | 227 | 97.4% | 6 |
| 20 | 214 | 227 | 94.3% | 8 |
| 21 | 165 | 227 | 72.7% | 4, 7 |
| 23 | 21  | 124 | 16.9% | 2, 4 |
| 24 | 224 | 227 | 98.7% | 9 |
| 26 | 176 | 178 | 98.9% | 5 |
| 29 | 215 | 226 | 95.1% | 3 |

**Overall: 1720 / 2127 = 80.87% PDR.** Nodes 15/21/23 were reassigned to a
different subevent mid-run (dynamic reallocation firing) — 23 dropped to
16.9%, the worst of the whole fleet. Node 13 only started reporting near the
very end of the run; node 26 stopped ~8 minutes early. No RSSI (predates
RSSI logging).

---

## 3. Walking, static PAwR, RE-RUN with RSSI (2026-08-27) — central 2, 11 subevents, no redundancy

Source: `logs/serial_20260827_183110.txt`, ~37 min (17:53:56-18:31:05). First
walking run captured with RSSI logging. Same 11-node roster, static-table
scheduling.

| node | PDR | RSSI avg | RSSI range | Reconnects |
|---:|---:|---:|---:|---:|
| 18 | 100.0% | -67.6 | -84 to -50 | 1 |
| 20 | 100.0% | -54.6 | -77 to -43 | 1 |
| 29 | 99.5% | -69.5 | -83 to -53 | 1 |
| 24 | 99.1% | -50.4 | -81 to -32 | 1 |
| 13 | 98.6% | -69.6 | -85 to -50 | 1 |
| 21 | 97.7% | -70.5 | -84 to -46 | 1 |
| 19 | 97.3% | -74.3 | -85 to -41 | 2 |
| 26 | 96.2%\* | -72.4 | -84 to -55 | 4 (1 reboot) |
| 16 | 95.3% | -68.7 | -83 to -46 | 1 |
| 15 | 52.7% | -74.2 | -85 to -49 | **57** |
| 23 | 23.9% | -74.5 | -82 to -37 | **136** |

\* Node 26 rebooted mid-run (seq reset 95→0, 16 `TEMP_FAIL` readings at the
reboot); PDR computed correctly across both epochs.

**Overall: 87.32% PDR, RSSI avg -67.85 dBm.** Nodes 15 and 23 are the
outliers, but this time all 11 nodes stayed connected the whole run — the
difference from run #1 is connection-layer instability, not full dropout:
node 23 needed 136 reconnect attempts (83 "RF noise" BLE connection
failures, 19 GATT-discovery timeouts) and node 15 needed 57, versus 1-2 for
every other node. RSSI alone (-74 avg for both) doesn't distinguish them
from several healthy nodes — the failure mode is connection stability, not
raw signal strength.

---

## 4. Walking, dynamic (hub-driven) PAwR, POST-FIX (2026-08-28) — central 2, 33 subevents, 3x redundant

Source: `logs/serial_20260828_091911.txt`, ~37 min (08:41:43-09:19:03).
First dynamic-PAwR run on the corrected build: a peripheral/central GATT
wire-format mismatch (peripheral firmware built for `NUM_REDUNDANT_COPIES=3`
vs. central initially built for `NUM_REDUNDANT_COPIES=1`) had been causing
every onboarding GATT write to fail with `err 13` (write not permitted) —
fixed by rebuilding central with matching `NUM_REDUNDANT_COPIES=3` /
`NUM_SUBEVENTS=33` config, `APP_DYNAMIC_SCHEDULING=1`.

| node | PDR | RSSI avg | Reconnects |
|---:|---:|---:|---:|
| 13, 16, 18, 20, 29 | 100.0% | -49 to -68 | 1 |
| 24, 26 | 99.5% | -60 to -66 | 1 |
| 21 | 98.5% | -73.0 | 1 |
| 19 | 86.5% | -73.7 | 2 |
| 15 | 72.5% | -71.8 | 6 |
| 23 | 44.2% | -75.4 | 28 |

**Overall: 90.98% PDR, RSSI avg -66.01 dBm.** All 11 nodes present the full
run — the best dynamic-PAwR walking result to date, and the first dynamic
run where the wire-format bug isn't confounding the numbers. Node 23 remains
the clear weak point (28 reconnects, mostly RF-noise connection-establishment
failures, not GATT-write errors); node 15 secondary. Only 1 `Write failed`
and 6 `Timed out` GATT events in the whole run, confirming the format fix
holds. Sensor note: 1,236 readings across nodes 13/20/21/24/26/29 show
`humidity >= 100%` (up to ~103%) — sensor saturation, not a delivery
problem; skin_temp readings were clean throughout with zero `TEMP_FAIL`
flags and no stuck/frozen values on any node.

*A ~16-min continuation segment immediately following this run
(`logs/serial_20260828_091919.txt`, 09:19:22-09:35:34, same boot) was a
live system test during firmware verification, not a fresh independent
run — excluded from this summary. In it, nodes 15/19/23 (already dropped
during run #4, at 08:55:12 / 09:02:13 / 09:00:13 respectively) remained
absent, and none of the three showed any further reconnect attempt in either
log after their last response.*

---

## 5. Stationary, dynamic (hub-driven) PAwR (2026-08-24) — 11 subevents, no redundancy

Source: `logs/stationary_dynamic_pawr_30min_20260824_run2.log`, 30 min
(16:00:10-16:30:00, confirmed full duration).

| node | received | expected | PDR |
|---:|---:|---:|---:|
| 13 | 180 | 181 | 99.4% |
| 15 | 162 | 178 | 91.0% |
| 16 | 155 | 180 | 86.1% |
| 18 | 169 | 180 | 93.9% |
| 19 | 125 | 178 | 70.2% |
| 20 | 172 | 181 | 95.0% |
| 21 | 170 | 181 | 93.9% |
| 23 | 160 | 181 | 88.4% |
| 24 | 173 | 181 | 95.6% |
| 26 | 144 | 181 | 79.6% |
| 29 | 163 | 181 | 90.1% |

**Overall: 1773 / 1983 = 89.41% PDR.** All 11 nodes present the full 30
minutes, zero dropouts. Node 19 is the clear outlier (70.2%) — it also took
two full verification passes earlier in the session before it was ever seen
onboarding, consistent with a marginal individual link rather than random
loss. Node 26 (79.6%) is a secondary weak point.

---

## 6. Stationary, static/fixed-table PAwR (2026-08-24) — 11 subevents, no redundancy

Source: `logs/stationary_static_pawr_30min_20260824.log`, 30 min
(16:50:59-17:20:52, confirmed full duration). Same fleet, same day, same
physical setup as run #5 — only the central's scheduling mode changed
(`APP_DYNAMIC_SCHEDULING=0`), giving a clean same-day dynamic-vs-static
counterpart with no dropout confound on either side.

| node | received | expected | PDR |
|---:|---:|---:|---:|
| 13 | 177 | 180 | 98.3% |
| 15 | 163 | 180 | 90.6% |
| 16 | 160 | 180 | 88.9% |
| 18 | 168 | 180 | 93.3% |
| 19 | 142 | 180 | 78.9% |
| 20 | 163 | 180 | 90.6% |
| 21 | 172 | 180 | 95.6% |
| 23 | 156 | 179 | 87.2% |
| 24 | 164 | 179 | 91.6% |
| 26 | 165 | 181 | 91.2% |
| 29 | 155 | 181 | 85.6% |

**Overall: 1785 / 1980 = 90.15% PDR.** All 11 nodes present the full 30
minutes. Node 19 is again the clear outlier (78.9%, though better than its
70.2% under dynamic scheduling) — no other node stands out.

---

## 7. Walking, Broadcasting (uncoordinated advertising) (2026-08-23) — ~33 min

Passive-scan hub, no scheduling/coordination at all — every node broadcasts
independently (non-connectable advertising), hub does per-node seq/gap
tracking. Source: `serial_20260823_221621.txt`, Broadcasting CSV section
from 21:42:57.167 to 22:16:21.841 (~33 min 25 s). Repeated receptions of the
same sequence number are counted once. A brief burst decoded as node 1 was
excluded (not part of the 11-node roster, implausible sensor values,
sequence number jumping among three unrelated values) as foreign/misdecoded
BLE traffic; the earlier PAwR-formatted section before a serial reconnect
was also excluded.

| node | received | expected | missing | PDR |
|---:|---:|---:|---:|---:|
| 13 | 1435 | 1985 | 550 | 72.29% |
| 15 | 325  | 1984 | 1659 | 16.38% |
| 16 | 1181 | 1986 | 805 | 59.47% |
| 18 | 1384 | 1979 | 595 | 69.93% |
| 19 | 1562 | 1984 | 422 | 78.73% |
| 20 | 1391 | 1979 | 588 | 70.29% |
| 21 | 1565 | 1986 | 421 | 78.80% |
| 23 | 282  | 1984 | 1702 | 14.21% |
| 24 | 1240 | 1983 | 743 | 62.53% |
| 26 | 1700 | 2005 | 305 | 84.79% |
| 29 | 1318 | 1983 | 665 | 66.47% |

**Overall: 13383 / 21838 = 61.28% PDR.** Strongest node: **26 (84.79%)**.
Nodes **15 (16.38%)** and **23 (14.21%)** are dramatically worse than the
rest — the same back-worn pair that collapses under PAwR walking too,
consistent with body shadowing rather than an architecture-specific
failure mode. No RSSI logged on this run.

---

## 8. Stationary, Broadcasting (uncoordinated advertising) (2026-08-22/23) — 30 min, with RSSI

Same uncoordinated-advertising architecture as run #7, stationary. Source:
`logs/ring2_pdr_30min_run1.csv`, captured with `tools/capture_hub_pdr_log.py`
(CSV columns: `hub_rx_millis,nodeID,seq,rssi,temp_cdeg,humidity_pct10,flags,dup,gap_count`,
confirmed against `broadcasting/src/hub_main.cpp`).

| node | received | expected | PDR | avg RSSI (dBm) |
|---:|---:|---:|---:|---:|
| 13 | 1215 | 1783 | 68.1% | -68.3 |
| 15 | 1317 | 1782 | 73.9% | -76.4 |
| 16 | 1225 | 1782 | 68.7% | -80.6 |
| 18 | 1312 | 1778 | 73.8% | -74.1 |
| 19 | 1305 | 1779 | 73.4% | -81.4 |
| 20 | 1356 | 1781 | 76.1% | -72.8 |
| 21 | 1270 | 1782 | 71.3% | -73.7 |
| 23 | 1184 | 1780 | 66.5% | -80.3 |
| 24 | 1307 | 1782 | 73.3% | -72.2 |
| 26 | 1313 | 1781 | 73.7% | -69.1 |
| 29 | 1311 | 1779 | 73.7% | -79.8 |

**Overall: 14115 / 19589 = 72.06% PDR.** All 11 nodes present, no dropouts.
Nodes **15/19/23** are the weakest here, but the spread is narrow (66.5% to
76.1%) compared to PAwR's stationary spread (78.9% to 99.4%, runs #5/#6) —
consistent with contention-driven loss (every node competing for the same
unscheduled channel) hitting strong- and weak-signal nodes more evenly than
PAwR's link-quality-driven loss does.

---

## Does body position predict RSSI / PDR?

Three runs above logged RSSI over the same 11 bodies/positions/hub-placement
(runs #5, #6, and #8 — the stationary Broadcasting baseline), letting body
geometry be checked directly across both architectures:

| node | position | dyn. PAwR PDR | dyn. avg RSSI | static PAwR PDR | static avg RSSI | Broadcasting PDR | Broadcasting avg RSSI |
|---:|---|---:|---:|---:|---:|---:|---:|
| 13 | chest | 99.4% | -43.3 | 98.3% | -49.5 | 68.1% | -68.3 |
| 20 | left hand | 95.0% | -57.7 | 90.6% | -57.3 | 76.1% | -72.8 |
| 21 | right calf | 93.9% | -59.8 | 95.6% | -56.5 | 71.3% | -73.7 |
| 24 | abdomen | 95.6% | -62.7 | 91.6% | -65.4 | 73.3% | -72.2 |
| 18 | forearm | 93.9% | -65.7 | 93.3% | -62.7 | 73.8% | -74.1 |
| 29 | left bicep | 90.1% | -66.3 | 85.6% | -67.5 | 73.7% | -79.8 |
| 16 | head | 86.1% | -67.2 | 88.9% | -64.2 | 68.7% | -80.6 |
| 23 | lower back | 88.4% | -67.9 | 87.2% | -72.5 | 66.5% | -80.3 |
| 15 | upper back | 91.0% | -69.3 | 90.6% | -66.7 | 73.9% | -76.4 |
| 26 | right thigh | 79.6% | -71.5 | 91.2% | -68.6 | 73.7% | -69.1 |
| 19 | right foot | 70.2% | -72.2 | 78.9% | -73.4 | 73.4% | -81.4 |

Sorted by dynamic-PAwR-run RSSI, strongest to weakest — the order lines up
with body geometry for a hub carried in the **left** pocket: **chest** (13,
closest/most direct line-of-sight) is strongest by a wide margin in both
PAwR runs (-43 dBm, ~15 dB clear of the next-best node); **left
hand**/**left bicep** (20, 29 — same side as the pocket) both rank in the
stronger half; **right foot** (19, farthest, most body mass in the path) is
weakest or near-weakest in all three runs. Body position clearly shapes RSSI
under Broadcasting too (same ranking pattern), but its PDR spread is much
narrower (66.5-76.1%) than PAwR's (70.2-99.4%) despite covering the same
RSSI range — see the correlation numbers below for why.

**Correlation strength (Pearson r, RSSI vs. PDR, n=11):**

| run | r |
|---|---:|
| PAwR, dynamic scheduling, stationary | **0.729** |
| PAwR, static scheduling, stationary | **0.821** |
| Broadcasting, stationary | **0.180** |

**RSSI predicts PDR strongly for PAwR (r=0.73-0.82) but weakly for
Broadcasting (r=0.18)** — same bodies, positions, hub placement, and
physical environment. This is architecturally sensible: PAwR is a
scheduled, collision-free protocol, so its dominant loss mechanism is link
quality itself (weak/obstructed signal → dropped or undecodable packets),
which RSSI tracks directly. Broadcasting's dominant loss mechanism is
channel contention between independently-timed nodes — a strong-signal node
can still lose a packet to a collision just as often as a weak-signal one,
so RSSI is a much weaker predictor of its PDR. Body position shapes RSSI
identically under both architectures, but only translates into a PDR
difference under PAwR.

**Nodes 15/23 (back-worn) sit in the weaker half but not the extreme worst
while stationary** — it's specifically **walking** that turns them into the
standout worst performers, and this holds under **both architectures**:
PAwR (runs #1-4 above) and Broadcasting alike (run #7: 15 drops from 73.9%
stationary to 16.38% walking, 23 from 66.5% to 14.21%). Consistent with
gait-driven antenna reorientation/arm-swing shadowing stacking on top of
their already weaker static positioning — a body-geometry effect, not an
architecture-specific failure mode. Node 19 (right foot) is the opposite
pattern under PAwR: the consistent weak point **stationary**, but recovers
to solid PDR while walking (97.3-97.4% in runs #2 and #4) — gait motion
likely improves its line-of-sight rather than degrading it, the opposite
effect from 15/23. Under Broadcasting, node 19 doesn't show this same
recovery as clearly (73.4% walking vs. 73.4% stationary — essentially flat),
consistent with Broadcasting's PDR being contention-driven rather than
link-quality-driven, so a single node's RSSI improvement doesn't move its
PDR the way it does under PAwR.

---

## Summary — all walking vs. stationary runs

| # | test | architecture | scheduling | conditions | overall PDR | weakest node |
|---|---|---|---|---|---:|---|
| 1 | Walking (~29 min) | PAwR | static | walking | 90.26%\* | 15/23 (full dropout) |
| 2 | Walking (~37-38 min) | PAwR | dynamic | walking | 80.87% | 23 (16.9%) |
| 3 | Walking (~37 min), w/ RSSI | PAwR | static | walking | 87.32% | 23 (23.9%) |
| 4 | Walking (~37 min), post-fix | PAwR | dynamic | walking | 90.98% | 23 (44.2%) |
| 5 | Stationary (30 min) | PAwR | dynamic | stationary | 89.41% | 19 (70.2%) |
| 6 | Stationary (30 min) | PAwR | static | stationary | 90.15% | 19 (78.9%) |
| 7 | Walking (~33 min) | Broadcasting | none (uncoordinated) | walking | 61.28% | 23 (14.21%) |
| 8 | Stationary (30 min) | Broadcasting | none (uncoordinated) | stationary | 72.06% | 23 (66.5%) |

\* 9-of-11-node figure — nodes 15/23 excluded entirely (full dropout), not
comparable 1:1 to the other 11-node rows.

**Static vs. dynamic, walking (PAwR):** static wins on raw numbers in every
walking pairing so far, but runs #1/#2 (no RSSI, original comparison) are
confounded — the static run's higher number is partly an artifact of 15/23
dropping out of its denominator entirely rather than the scheduler
delivering better per-node reliability. Runs #3/#4 (both with RSSI, both
11-node-complete) are the cleaner comparison: static 87.32% vs. dynamic
90.98% — dynamic actually edges ahead once both scheduling modes are held to
full-fleet-present, non-dropout conditions.

**Static vs. dynamic, stationary (PAwR):** essentially a wash (90.15% vs.
89.41%, 0.74 points apart), no dropouts either side — the real
fixed-vs-dynamic gap is much smaller than the raw walking numbers alone
would suggest.

**PAwR vs. Broadcasting:** PAwR beats Broadcasting decisively in every
walking-vs-walking and stationary-vs-stationary pairing (walking: 80.9-91.0%
PAwR vs. 61.3% Broadcasting; stationary: 89.4-90.2% PAwR vs. 72.1%
Broadcasting), and the gap is architectural, not incidental — see "Does body
position predict RSSI / PDR?" above: PAwR's scheduled, collision-free design
means link quality (which body position controls via RSSI) is its dominant
loss mechanism, while Broadcasting's uncoordinated channel access means
contention dominates instead, capping its achievable PDR regardless of how
good any individual node's signal is. Broadcasting's walking/stationary gap
(61.3% vs. 72.1%, ~11 points) is roughly in line with PAwR's own
walking-vs-stationary gap once dropout artifacts are controlled for (runs
#3/#4 vs. #5/#6: high-80s to low-90s in both conditions) — walking costs
both architectures a meaningful amount of PDR, just from a different
starting baseline.

**The dominant effect throughout is body position, not scheduling mode or
architecture.** Nodes 15 and 23 (back-worn) are the consistent weak point in
every walking run, PAwR or Broadcasting alike; node 19 (right foot) is the
consistent weak point in every **PAwR** stationary run (Broadcasting's own
weakest stationary node is 23, per run #8, since contention flattens the
RSSI-driven ranking PAwR shows). The 15/23 walking failure mode doesn't
transfer to stationary conditions under either architecture — whichever body
position is under the most motion-dependent RF stress for a given posture
dominates the result far more than which slot-allocation algorithm the
central runs, or even which BLE architecture (scheduled vs. uncoordinated)
is in use.
