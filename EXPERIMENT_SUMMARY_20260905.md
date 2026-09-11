# Experiment Summary — 2026-09-05

Covers today's work: porting the connection-deadlock fix to dynamic PAwR,
30-minute sitting comparisons (dynamic vs. static PAwR), two walking
experiments captured 2026-09-04 (analyzed today), and standing up the real
Broadcasting architecture after an earlier branch-name mix-up.

## Master matrix — architecture × posture × hub position

Consolidated across this doc, `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md`,
`BROADCASTING_SITTING_SUMMARY.md`, and `BROADCASTING_RESULTS_SUMMARY.md`.

| Architecture | Sitting | Walking, front pocket | Walking, back/butt pocket |
|---|---|---|---|
| Dynamic PAwR | ✅ 2 runs (below) | ✅ `serial_20260904_184714.txt` (below) | ✅ `serial_20260904_192210.txt` (below) |
| Static PAwR | ✅ 2 runs, `sitting_static_pawr_serial_20260905.txt` / `_run2.txt` (below) | ✅ `serial_20260903_192835.txt` — see `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` | ✅ `serial_20260903_200307.txt` ("butt-pocket") — see `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` |
| Broadcasting | ✅ 2 runs — see `BROADCASTING_SITTING_SUMMARY.md` | ✅ `serial_20260902_184209.txt` — see `BROADCASTING_RESULTS_SUMMARY.md` | ✅ 2 runs: `serial_20260905_180847.txt`, `serial_20260906_152805.txt` (below) |

The matrix is complete — every architecture × posture × hub-position cell
has at least one captured run. Section 6 adds a second back-pocket
replication for both dynamic PAwR and Broadcasting (2026-09-06).

## Experiment inventory (what's been run so far)

| # | Experiment | Architecture | Posture | Hub position | Log(s) |
|---|---|---|---|---|---|
| 1 | Walking, front pocket | Dynamic PAwR | Walking | Front pocket | `logs/serial_20260904_184714.txt` |
| 2 | Walking, back pocket | Dynamic PAwR | Walking | Back pocket | `logs/serial_20260904_192210.txt` |
| 3 | Sitting, dynamic PAwR | Dynamic PAwR | Sitting | n/a (central 3, fixed) | `logs/sitting_dynamic_pawr_serial_20260905.txt` (+ discarded partial) |
| 4 | Sitting, static PAwR, run 1 | Static PAwR | Sitting | n/a | `logs/sitting_static_pawr_serial_20260905.txt` |
| 5 | Sitting, static PAwR, run 2 | Static PAwR | Sitting | n/a | `logs/sitting_static_pawr_serial_20260905_run2.txt` (2 earlier attempts discarded — nodes not powered on / node 41 not yet reflashed) |
| 6 | Walking, front pocket (prior session) | Static PAwR | Walking | Front-left pocket | `logs/serial_20260903_192835.txt` — see `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` |
| 7 | Walking, butt pocket (prior session) | Static PAwR | Walking | Left-butt pocket | `logs/serial_20260903_200307.txt` — see `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` |
| 8 | Sitting, run 1 (prior session) | Broadcasting | Sitting | n/a | `logs/dumps/hub4_30min_broadcasting_20260902_164755.csv` — see `BROADCASTING_SITTING_SUMMARY.md` |
| 9 | Sitting, run 2 (prior session) | Broadcasting | Sitting | n/a | `logs/dumps/hub4_30min_broadcasting_run2_20260902_173935.csv` — see `BROADCASTING_SITTING_SUMMARY.md` |
| 10 | Walking (prior session) | Broadcasting | Walking | Front pocket | `logs/serial_20260902_184209.txt` — see `BROADCASTING_RESULTS_SUMMARY.md` |
| 11 | Broadcasting fleet bring-up | Broadcasting | n/a | n/a | in progress — 2 hubs confirmed booting; 8/44 nodes reflashed with corrected firmware so far |
| 12 | Walking, back pocket | Broadcasting | Walking | Back pocket | `logs/serial_20260905_180847.txt` (below) |
| 13 | Walking, back pocket (replication) | Dynamic PAwR | Walking | Back pocket | `logs/serial_20260906_173148.txt` (below, section 6) |
| 14 | Walking, back pocket (replication) | Broadcasting | Walking | Back pocket | `logs/serial_20260906_152805.txt` (below, section 6) |

Experiments 1-2 and 12 predate/overlap today's session (captured
2026-09-04/05) but hadn't been analyzed yet — full breakdown below.
Experiments 6-10 are prior-session work, documented in their own summary
files, included here only for the master matrix above. Experiment 11 is
fleet setup, not a data-collection run. Experiments 13-14 were captured
2026-09-06 (a later session) and analyzed in section 6, added to keep this
doc as the single running log rather than starting a new file per day.

## 1. Ported connection-deadlock fix + watchdog + storage format to dynamic PAwR

Branch `dynamic-pawr-deadlock-fix` (based on `dynamic-pawr-4rig`), committed
as `709e477`.

**Central**, ported from `main`'s 2026-08-31 fix:
- `connect_pending` gate — the dynamic branch was missing this and would have
  hit the same NULL-`default_conn` crash on every idle scan cycle.
- Bounded 25s `k_poll()` timeout replacing `K_FOREVER`, with a real
  cancel-confirmation wait (`k_sem_take` on `sem_disconnected`, not a blind
  sleep) — recovers a stalled connection attempt (peripheral walked out of
  range mid-onboarding) in seconds instead of hanging the whole central.
- Scan-restart-on-failure in `device_found()`, and a scan-start retry loop
  with watchdog-reset fallback in `main()`.
- Dynamic-mode-specific addition: releases the allocated slot
  (`slot_taken[pending_slot] = false`) when a stalled connection is
  canceled, mirroring the existing write-failure paths — the fixed-table
  version doesn't need this since it has no slot pool to leak.

**Peripheral**, closing the gap with static PAwR:
- Hardware watchdog (`common/watchdog.c/.h`), bounded 30s-slice polling
  instead of `K_FOREVER` on both long waits.
- Block-batched flash-storage format (`RECORDS_PER_BLOCK=16` per FCB entry)
  ported from `main`, ~3x capacity before wraparound vs. the branch's
  original one-record-per-entry format.

Verified on real hardware: central 3 survived repeated real `0x3E`
connection failures without wedging, in both dynamic and fixed-table mode.
Confirmed building clean in both `APP_DYNAMIC_SCHEDULING=0` and `=1`.

## 2. Walking experiments — dynamic PAwR, hub position (front vs. back pocket)

Captured 2026-09-04, analyzed today. ~31 minutes each, 11-node roster (30,
31, 32, 33, 34, 35, 37, 38, 40, 41, 42), subject walking with the hub
carried in a front vs. back pocket between runs.

**Far noisier than any sitting run** — walking induces much more
range/orientation-driven connection churn than sitting still.

### Front pocket (`logs/serial_20260904_184714.txt`)

242 disconnect events: 189x `0x3E` (connection failed to establish), 50x
`0x08` (timeout), 2x `0x16`, 1x `0x22`.

| Node | PDR | Avg RSSI | Notes |
|---|---|---|---|
| 30 | 94.1% | -75.9 | |
| 31 | 100.0% | -66.9 | |
| 32 | 100.0% | -63.7 | |
| 33 | 98.4% | -73.7 | |
| 34 | 33.5%* | -78.1 | Scattered dropouts (largest gap 23), real intermittent trouble |
| 35 | 100.0% | -63.7 | |
| 37 | 100.0% | -43.3 | Strongest signal this run |
| 38 | 99.5% | -62.5 | |
| 40 | **6.9%** | -72.5 | Effectively failed — only 4 real readings over 31 min, spread across huge gaps (37, 16) |
| 41 | 98.9% | -74.5 | |
| 42 | 99.5% | -62.1 | |

### Back pocket (`logs/serial_20260904_192210.txt`)

280 disconnect events: 208x `0x3E`, 66x `0x08`, 4x `0x16`, 2x `0x22` —
somewhat worse than front pocket on raw disconnect count.

| Node | PDR | Avg RSSI | Notes |
|---|---|---|---|
| 30 | 86.6% | -76.1 | |
| 31 | 50.3%* | -80.7 | Weak signal, real degraded reception throughout |
| 32 | 97.8% | -70.9 | |
| 33 | 99.5% | -72.0 | |
| 34 | 100.0% | -65.8 | Clean this run — contrast with front-pocket's 33.5% |
| 35 | 64.6%* | -72.6 | One large gap (102) mid-walk |
| 37 | 56.5%* | -67.2 | One large gap (143) mid-walk; also `HUMIDITY_FAIL` x409 |
| 38 | 98.9% | -65.8 | |
| 40 | 100.0% | -62.5 | Clean this run — contrast with front-pocket's 6.9% |
| 41 | 97.3% | -73.9 | |
| 42 | **4.5%** | -78.0 | Effectively failed — only 2 real readings over 31 min |

*Raw PDR% for 34 (front)/31/35/37 (back) reflects a mix of scattered loss
and a few large gaps — see the log-level analysis for exact gap sizes.

### Takeaways

- **Nodes 40 and 42 each failed almost completely, but in opposite runs**
  (40 in front-pocket, 42 in back-pocket) — this looks like a per-node
  physical/antenna issue interacting with hub orientation, not a general
  "front worse than back" or "back worse than front" effect. Worth checking
  whether 40/42's mounting or antenna orientation on the body is unusual
  relative to the other nodes.
- **Node 34 flipped**: bad in front-pocket (33.5%), clean in back-pocket
  (100%) — the opposite direction from 40/42, reinforcing that this is
  about specific node/hub geometry on a given run, not a consistent
  front-vs-back winner.
- **Node 37's humidity sensor fault also shows up here** (`HUMIDITY_FAIL`
  x409, back-pocket run) — consistent with the hardware fault confirmed
  independently in the sitting experiments (see below), on both dynamic and
  static PAwR.
- Neither run's overall PDR pattern cleanly favors one hub position over
  the other — front pocket had fewer total disconnects (242 vs 280) but
  back pocket didn't have an equivalent to front's near-total node-40
  failure. A single run per position isn't enough to call a winner; the
  per-node variance dominates whatever position effect exists.

## 3. Sitting experiments — dynamic vs. static PAwR

All runs: central 3, 11-node roster (30, 31, 32, 33, 34, 35, 37, 38, 40, 41,
42), 30 minutes, subjects seated.

### Dynamic PAwR (`logs/sitting_dynamic_pawr_serial_20260905.txt`)

First attempt stalled silently ~15 min in (capture-tool issue, not a board
crash — board stayed connected and enumerated throughout); restarted clean.
Final run: 0 resets/disconnects.

| Node | PDR | Avg RSSI | Notes |
|---|---|---|---|
| 30 | 100.0% | -67.9 | |
| 31 | 100.0% | -68.0 | |
| 32 | 100.0% | -51.8 | |
| 33 | 99.4% | -64.5 | |
| 34 | 97.8% | -77.2 | Weakest signal |
| 35 | 100.0% | -53.7 | |
| 37 | 100.0% | -50.9 | `HUMIDITY_FAIL` x529 |
| 38 | 100.0% | -47.3 | |
| 40 | 99.4% | -74.2 | |
| 41 | 99.2% | -74.3 | Partial coverage (range excursion mid-run) |
| 42 | 100.0% | -63.4 | |

### Static PAwR, run 1 (`logs/sitting_static_pawr_serial_20260905.txt`)

6 disconnect events total (2x `0x3E`, 3x `0x08`, plus normal).

| Node | PDR | Avg RSSI | Notes |
|---|---|---|---|
| 30 | 94.6% | -72.2 | |
| 31 | 97.8% | -68.2 | |
| 32 | 97.8% | -48.7 | |
| 33 | 100.0% | -63.6 | |
| 34 | ~high* | -78.8 | One 226-seq dropout dominates raw PDR number |
| 35 | 97.8% | -60.5 | |
| 37 | 99.4% | -48.4 | `HUMIDITY_FAIL` x531 |
| 38 | 97.8% | -49.7 | |
| 40 | 88.6% | -77.3 | Weakest reception this run |
| 42 | 98.9% | -62.3 | |

*Node 41 not yet reflashed at this point — missing from run 1.

### Static PAwR, run 2 (`logs/sitting_static_pawr_serial_20260905_run2.txt`)

Two earlier attempts invalidated and discarded (3 nodes not powered on;
node 41 not reflashed yet — see `_INVALID_*` files). Real run, all 11
nodes present including reflashed node 41.

**92 total disconnect events** (38x `0x08`, 42x `0x3E`, 7x `0x16`, 5x
`0x22`) — a dramatically noisier RF/onboarding environment than run 1,
same firmware. Not a regression; a real environmental difference.

| Node | PDR | Avg RSSI | Notes |
|---|---|---|---|
| 30 | 79.3% | -77.3 | New scattered loss (14 gaps) |
| 31 | 93.8% | -71.4 | |
| 32 | 93.8% | -63.7 | |
| 33 | 93.8% | -64.8 | |
| 34 | 16.0%* | -76.8 | Another huge dropout (292-seq gap) |
| 35 | 93.8% | -54.5 | |
| 37 | 100.0% | -44.5 | `HUMIDITY_FAIL` x540 |
| 38 | 100.0% | -54.8 | |
| 40 | 36.9%* | -75.3 | Two large gaps (69, 38) |
| 41 | 46.2%* | -73.1 | `HUMIDITY_FAIL` x143 (new fault); one 70-gap + re-onboarding |
| 42 | 100.0% | -61.9 | |

*Raw PDR% for 34/40/41 driven by single large dropouts, not scattered loss.

### Dynamic vs. static PAwR, sitting — summary comparison

Averages exclude nodes with a single large dropout distorting the raw PDR%
(see notes above and in each run's table), since including them would
conflate "this node had one bad stretch" with genuine link-quality
differences between conditions.

| Condition | Avg PDR (excl. dropout-affected nodes) | Avg RSSI |
|---|---|---|
| Dynamic PAwR, sitting | 99.6% (all 11 nodes — no dropouts this run) | -63.0 |
| Static PAwR, sitting run 1 | 97.0% (excl. 34, 41 missing) | -63.0 |
| Static PAwR, sitting run 2 | 94.3% (excl. 34, 40, 41) | -65.3 |

Sitting is close to a wash between dynamic and static PAwR — all three runs
land in a tight 94–100% band once each run's own dropout-affected outliers
are excluded, unlike the walking comparisons where the two modes visibly
diverge. This tracks with `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md`'s broader
finding that most nodes sit at or near ceiling PDR while stationary
regardless of scheduling mode — the primary+backup redundant-slot mechanism
both modes share is doing the real work here, and dynamic vs. static
allocation barely matters when nothing is moving. Static run 2's noisier RF
environment (92 disconnects vs. run 1's 6, see above) is visible as a real
~3-point PDR dip and ~2 dB weaker RSSI relative to run 1, showing up as
run-to-run environmental variance layered on top of the architecture
comparison, not a static-vs-dynamic effect.

### Takeaways

- **Node 34** is the consistent weak point across every run (dynamic and
  both static runs) — worst-or-near-worst RSSI every time, plus real
  extended dropouts. Worth checking its physical placement/body shadowing.
- **Node 37's humidity sensor fault is confirmed hardware-level**, not
  scheduling-related — near-identical fail counts (529/531/540) regardless
  of mode or run.
- **Node 41's humidity sensor is also now faulty** (confirmed on reflash,
  143 fails in its one valid run) — a second hardware fault, previously
  masked by node 41 being absent from earlier runs.
- No central crashes in any run — the deadlock/watchdog fixes held up
  under both scheduling modes and under a genuinely noisy RF period.
- Static run 2's much higher disconnect count shows real run-to-run
  environmental variance; a single 30-minute sample isn't enough to call a
  definitive dynamic-vs-static winner from today's data alone.

## 4. Broadcasting architecture — corrected after a branch-name mix-up

**What went wrong:** the git branch `broadcasting-baseline` is actually a
Zephyr GATT store-and-forward redesign (`SCAN`/`INIT`/`DOWNLOAD`/`STOP`
console commands, one connection at a time) — unrelated to this project's
real Broadcasting architecture, despite the misleading name. Built and
flashed this by mistake for the central and 5 nodes before catching it.

**The real Broadcasting architecture** lives in the untracked `broadcasting/`
directory — a separate PlatformIO/Arduino project (not `west`/Zephyr),
using the original Nordic SoftDevice S140 stack: independent,
connectionless BLE advertising per node, passive-scanning hub, no
onboarding/scheduling step at all. Built via `pio run -e hub` /
`pio run -e xiao_nrf52840` (see `broadcasting/README.md`), or in bulk via
`tools/Build-BroadcastingFleet.ps1 -NodeIds <list>`.

**Corrected and flashed today:**
- 2 hub boards, both confirmed booting with the real firmware (`BLE Hub —
  passive scan`, CSV header `hub_rx_millis,nodeID,seq,rssi,temp_cdeg,
  humidity_pct10,flags,dup,gap_count`).
- All 44 nodes' firmware built (`broadcasting/build_node<N>/firmware.uf2`).
  Node 42's first build attempt failed with a transient library-link error
  (same class of flaky failure node 3 hit once too) — resolved cleanly on
  a plain retry, not a real code issue.
- Nodes physically reflashed with the correct firmware so far: 1, 2, 3, 4,
  5, 6, 7 (8 in progress).

**Still to do:** reflash the remaining nodes (9, 11, 12, 13, 15, 16, 18,
19, 20, 21, 23, 24, 26, 29, 30–35, 37, 38, 40–43, 45, 47, 50, 51, 55–58,
61, 63) with the corrected firmware, then run the actual Broadcasting
comparison experiment.

## 5. Broadcasting, walking, back pocket — completing the matrix

`logs/serial_20260905_180847.txt`, ~30 min (17:38–18:08), hub CSV format
(`hub_rx_millis,nodeID,seq,rssi,temp_cdeg,humidity_pct10,flags,dup,gap_count`).
Same 11-node roster as the dynamic/static PAwR walking tests (30, 31, 32,
33, 34, 35, 37, 38, 40, 41, 42) rather than the earlier Broadcasting
sitting/walking roster (43–63) — a different physical node set, so this
isn't a controlled swap against the other Broadcasting runs, only against
the PAwR walking runs on the same roster.

PDR computed from the hub's own `gap_count` field: non-dup received /
(non-dup received + gap_count) — same method `BROADCASTING_RESULTS_SUMMARY.md`
uses for its walking run. A handful of stray low-count node IDs (1, 2, 3,
5, 6, 7, 8, 9, 12, 15, 18, 19, 20, 23, 29 — 1–13 packets each) appear in
the raw log; excluded from the table below as almost certainly corrupted/
misdecoded advertisements (BLE has no link-layer CRC visible at this parse
level, and Broadcasting's own wire format has no app-level CRC either — see
`common/pawr_protocol.h`/`broadcasting/README.md`), not real nodes.

This run's physical nodes (30–42) are different hardware from Broadcasting's
other documented runs (43–63), so a **node-ID comparison against
Broadcasting's own prior data would be meaningless**. Both node sets were
worn at the same 11 body positions, though (per `gui/body_mapping.json`'s
position legend, `BodyPart` list in `gui/sensor_gui.py`), so the table below
is by **body position**, using node 30–42's mapping as already published in
`PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` — this lets a real comparison happen
against both the PAwR walking runs (same physical nodes) and Broadcasting's
own sitting/walking runs (same body positions, different nodes). Note
`gui/body_mapping.json` has been edited multiple times since these docs were
written (confirmed via `git log`), so the position labels used here are the
ones already on record in each source doc, not freshly re-derived from the
current mapping file.

| Body position | Node (this run) | PDR (gap-based) | Avg RSSI | Notes |
|---|---|---|---|---|
| Shin & Calf | 30 | **10.7%** | -88.9 | Near-total failure |
| Head / Forehead | 31 | **3.0%** | -87.9 | Near-total failure |
| Left Hand | 32 | 67.4% | -82.7 | |
| Left Upper Arm | 33 | 64.6% | -85.1 | |
| Upper Back | 34 | 54.4% | -83.7 | |
| Upper Chest | 35 | 80.3% | -84.9 | Best this run |
| Abdomen | 37 | 76.7% | -83.4 | |
| Left Forearm | 38 | 82.3% | -75.4 | Strongest signal, best PDR |
| Lower Back | 40 | 74.5% | -84.7 | |
| Right Foot | 41 | 70.7% | -83.3 | |
| Right Thigh | 42 | 57.7% | -81.6 | |

**Average PDR (all 11): 58.4%** — **69.8% excluding Shin & Calf / Head-
Forehead.** **Average RSSI: -83.8 dBm** (uniformly weak — every position in
the -75 to -89 range, unlike PAwR's walking runs where several positions
stay above -65 dBm).

### Comparison against the PAwR walking runs (same physical nodes)

| Condition | Avg PDR (all 11) | Avg RSSI |
|---|---|---|
| Dynamic PAwR, front pocket | 84.6% | -67.0 |
| Dynamic PAwR, back pocket | 77.8% | -71.4 |
| Static PAwR, front-left pocket | 83.6% | -66.5 |
| Static PAwR, butt pocket | 93.6% | -67.9 |
| **Broadcasting, back pocket** | **58.4%** | **-83.8** |

(Dynamic PAwR row averages computed here from section 2's per-node table,
not stated explicitly there.)

### Comparison against Broadcasting's own runs, by body position (different nodes, same positions)

| Body position | Broadcasting sitting run 1 | Broadcasting sitting run 2 | Broadcasting walking, back pocket (this run) |
|---|---|---|---|
| Head / Forehead | 74.2% | 72.4% | **3.0%** |
| Upper Chest | 73.4% | 77.8% | 80.3% |
| Abdomen | 73.4% | 73.3% | 76.7% |
| Upper Back | 52.7% | 66.1% | 54.4% |
| Lower Back | 72.4% | 52.4% | 74.5% |
| Left Upper Arm | 79.2% | 78.1% | 64.6% |
| Left Forearm | 80.9% | 69.3% | 82.3% |
| Left Hand | 72.5% | 73.9% | 67.4% |
| Right Thigh | 71.5% | 72.2% | 57.7% |
| Shin & Calf | 63.5% | 78.8% | **10.7%** |
| Right Foot | 56.4% | 69.2% | 70.7% |

### Broadcasting front-pocket vs. back-pocket walking, by body position

`serial_20260902_184209.txt` (11-node roster 43–63) was confirmed
front-pocket. Different physical nodes from this back-pocket run, but same
11 body positions (`BROADCASTING_SITTING_SUMMARY.md`'s mapping), so this is
a genuine Broadcasting-only front-vs-back comparison:

| Body position | Front pocket PDR (node 43–63 run) | Back pocket PDR (this run, node 30–42) |
|---|---|---|
| Left Hand | 78.4% | 67.4% |
| Left Forearm | 62.2% | 82.3% |
| Left Upper Arm | 84.1% | 64.6% |
| Upper Back | **1.6%** | 54.4% |
| Abdomen | 87.1% | 76.7% |
| Upper Chest | 73.9% | 80.3% |
| Lower Back | **0.8%** | 74.5% |
| Right Thigh | 78.9% | 57.7% |
| Shin & Calf | 63.1% | **10.7%** |
| Head / Forehead | 50.0% | **3.0%** |
| Right Foot | 42.9% | 70.7% |

This is the clearest evidence yet of a genuine hub-position effect, not just
node-to-node noise: **moving the hub from front to back pocket flips which
positions fail**. Front-pocket wrecks Upper Back and Lower Back (1.6%,
0.8% — near-total silence, the classic torso-shadowing pattern also seen in
PAwR's front-pocket runs) while leaving Head/Forehead and Shin & Calf merely
mediocre (50.0%, 63.1%). Back-pocket does the reverse: Upper Back and Lower
Back recover to a mediocre-but-alive 54.4%/74.5%, while Head/Forehead and
Shin & Calf collapse to near-total silence (3.0%, 10.7%). Every other
position stays in a broadly similar 57–87% band regardless of hub position.
This exactly mirrors `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md`'s finding for
PAwR ("moving the hub to the back pocket flips which positions are
shadowed") — confirming the same underlying body-shadowing mechanism drives
both architectures, Broadcasting just has no redundant-slot mechanism to
paper over it, so the effect shows up as much starker PDR swings.

### Takeaways

- **Broadcasting's walking PDR (58.4%) is markedly worse than either PAwR
  mode on the same physical nodes** (76.8–93.6%), and RSSI is uniformly
  15–20 dB weaker across every position, not just the failing ones —
  consistent with Broadcasting's lack of the redundant primary+backup
  subevent slots PAwR relies on (`PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md`
  notes this redundancy "is doing real work" for PAwR's near-ceiling PDR
  even while walking).
- **By body position, by itself this run's failure pattern looks like it
  breaks from every prior walking test** (here it's Shin & Calf and Head/
  Forehead that nearly vanish, not a back position) — but the front-vs-back
  Broadcasting comparison right above resolves this: front-pocket fails
  Upper Back/Lower Back exactly like every other front-pocket run in this
  project, and back-pocket simply flips which positions get shadowed, this
  time landing on Head/Forehead and Shin & Calf instead. Same underlying
  mechanism as PAwR's front/back flip, not a new one — it's the specific
  *pair* of positions the flip lands on that differs run to run, not
  whether a flip happens.
- **By-position comparison against Broadcasting's own sitting runs is
  revealing**: Upper Back and Lower Back are consistently mediocre-to-weak
  across *all three* Broadcasting runs (52–74%, no clear sitting-vs-walking
  gap) — reinforcing that those two positions are shadowed by the hub
  regardless of motion, matching `BROADCASTING_SITTING_SUMMARY.md`'s own
  observation that "torso shadowing from the hub's chest/waist position
  persists regardless of motion." Head/Forehead and Shin & Calf, by
  contrast, are perfectly fine sitting (~63–78%) and only collapse while
  walking with the hub in the back pocket — a motion-specific effect on
  those two positions, not a static shadowing one.
- Unlike the PAwR walking runs (where most non-failing positions sit at or
  near 100% PDR thanks to redundant slots), Broadcasting's non-failing
  positions here still only reach 54–82% — the "middle tier" PAwR's
  redundancy effectively erases stays visible in Broadcasting's numbers,
  same conclusion as the raw-node view.

## 6. Repeat back-pocket runs — dynamic PAwR and Broadcasting (2026-09-06)

Same 11-node roster (30, 31, 32, 33, 34, 35, 37, 38, 40, 41, 42, person_3
mapping) as every other run in this doc, hub carried in the back pocket for
both. Purpose: a same-node, same-position **replication** of section 5's
back-pocket runs, not new conditions — both architectures already had a
back-pocket data point; this doubles the sample.

- Dynamic PAwR: `logs/serial_20260906_173148.txt`, ~30 min (17:01–17:32).
  156 disconnect events (84x `0x08`, 61x `0x3E`, 11x `0x16`). No central
  crashes/watchdog resets. PDR computed per node from unique response
  sequence numbers over expected span, **reboot-aware**: nodes 40, 41, and
  42 each re-onboarded mid-run (sequence counter visibly reset — e.g. node
  42 jumped from seq 151 back to seq 0 at 17:12:36), so PDR is computed
  per contiguous seq-epoch and summed rather than treating the whole run as
  one span (the naive single-span method understates these three nodes by
  5-14 points).
- Broadcasting: `logs/serial_20260906_152805.txt`, ~30 min (14:57–15:28),
  same hub CSV format as section 5
  (`hub_rx_millis,nodeID,seq,rssi,temp_cdeg,humidity_pct10,flags,dup,gap_count`).
  PDR computed the same way as section 5: non-dup received /
  (non-dup received + gap_count). All rows matched the 11-node roster this
  time — no stray/corrupted node IDs to exclude.

### Per-node results, by body position

| Body position | Node | Dynamic PAwR PDR | Dynamic avg RSSI | Broadcasting PDR | Broadcasting avg RSSI | Notes |
|---|---|---|---|---|---|---|
| Shin & Calf | 30 | 79.3% | -78.6 | 17.3% | -87.8 | Weak in both |
| Head / Forehead | 31 | **14.8%** | -82.0 | **1.3%** | -86.0 | Near-total failure in both — 71 onboarding retries this run, by far the most of any node |
| Left Hand | 32 | 90.7% | -74.5 | 62.8% | -85.8 | |
| Left Upper Arm | 33 | 86.9% | -76.3 | 67.1% | -84.5 | |
| Upper Back | 34 | 100.0% | -70.2 | 75.7% | -80.2 | Clean this run — contrast with its usual weak-point reputation (see section 3) |
| Upper Chest | 35 | 100.0% | -74.0 | 55.4% | -86.4 | |
| Abdomen | 37 | 98.9% | -74.1 | 39.3% | -87.0 | `TEMP_FAIL`/`HUMIDITY_FAIL` x470 (confirmed hardware fault, see section 3) |
| Left Forearm | 38 | 95.6% | -67.0 | 84.6% | -77.6 | Strongest RSSI both runs |
| Lower Back | 40 | 94.5% | -73.8 | 83.0% | -79.9 | Re-onboarded once (dynamic PAwR) |
| Right Foot | 41 | 85.7% | -73.3 | 40.2% | -85.8 | Re-onboarded once (dynamic PAwR) |
| Right Thigh | 42 | 95.4% | -66.6 | 29.9% | -85.7 | Re-onboarded once (dynamic PAwR) — naive single-span PDR would read 81.6%, understating it by 14 points |

**Average PDR (all 11): dynamic PAwR 85.6%, Broadcasting 50.6%.** Average
RSSI: dynamic PAwR -73.7 dBm, Broadcasting -84.3 dBm. Same ~30-35 point PDR
gap and ~10 dB RSSI gap between architectures as every prior comparison in
this doc — dynamic PAwR's redundant primary+backup slots continue to cover
for weak links that leave Broadcasting exposed.

### Replication check against the 2026-09-04/05 back-pocket runs (same nodes, same positions)

| Body position | Node | Dynamic PAwR 09-04 | Dynamic PAwR 09-06 | Δ | Broadcasting 09-05 | Broadcasting 09-06 | Δ |
|---|---|---|---|---|---|---|---|
| Shin & Calf | 30 | 86.6% | 79.3% | -7.3 | 10.7% | 17.3% | +6.6 |
| Head / Forehead | 31 | 50.3% | 14.8% | -35.5 | 3.0% | 1.3% | -1.7 |
| Left Hand | 32 | 97.8% | 90.7% | -7.1 | 67.4% | 62.8% | -4.6 |
| Left Upper Arm | 33 | 99.5% | 86.9% | -12.6 | 64.6% | 67.1% | +2.5 |
| Upper Back | 34 | 100.0% | 100.0% | 0.0 | 54.4% | 75.7% | +21.3 |
| Upper Chest | 35 | 64.6% | 100.0% | +35.4 | 80.3% | 55.4% | -24.9 |
| Abdomen | 37 | 56.5% | 98.9% | +42.4 | 76.7% | 39.3% | -37.4 |
| Left Forearm | 38 | 98.9% | 95.6% | -3.3 | 82.3% | 84.6% | +2.3 |
| Lower Back | 40 | 100.0% | 94.5% | -5.5 | 74.5% | 83.0% | +8.5 |
| Right Foot | 41 | 97.3% | 85.7% | -11.6 | 70.7% | 40.2% | -30.5 |
| Right Thigh | 42 | 4.5% | 95.4% | +90.9 | 57.7% | 29.9% | -27.8 |
| **Average** | | **77.8%** | **85.6%** | +7.8 | **58.4%** | **50.6%** | -7.8 |

### Takeaways

- **Head/Forehead (node 31) is now confirmed as a real structural weak
  point with the hub in the back pocket, not run-to-run noise** — it's the
  worst or near-worst performer in all four back-pocket runs across both
  architectures (50.3%/14.8% dynamic, 3.0%/1.3% Broadcasting), the only
  position that stays consistently bad across a full replication. This
  contrasts with every other position, which swings 20-90 points between
  runs (see table above) — Head/Forehead's badness is structural
  (distance/orientation from a back-pocket hub), the others' variance is
  environmental/incidental.
- **Every other position is dominated by run-to-run variance, not a stable
  per-position effect.** Node 42 (Right Thigh) went from near-total failure
  (4.5%) to near-ceiling (95.4%) on dynamic PAwR between runs; node 37
  (Abdomen) swung the opposite way on Broadcasting (76.7% → 39.3%). This is
  the same conclusion section 2 already drew from the front/back pocket
  comparison — per-node/per-run variance dominates whatever position effect
  exists, except for the one structural outlier (Head/Forehead) that
  survives replication.
- **Upper Back (node 34) broke its own pattern this run** — flagged in
  section 3 as "the consistent weak point across every [sitting] run," it
  posted 100.0%/75.7% here, its best results in this doc. Likely reflects
  natural variance in exact torso/strap positioning between sessions rather
  than a contradiction of the shadowing mechanism itself.
- **Dynamic PAwR vs. Broadcasting gap replicates cleanly**: 85.6% vs 50.6%
  average PDR this run, essentially the same ~27-30 point gap as section 5's
  77.8% vs 58.4% — confirms the redundant-slot advantage is a stable,
  repeatable effect and not an artifact of a single run.
- **Three mid-run re-onboards on dynamic PAwR** (nodes 40, 41, 42, all
  detected via sequence-counter resets in the log) with **zero central
  crashes or watchdog resets** — consistent with section 1's deadlock/
  watchdog fix holding up, this time under a live ~30-minute walking-with-
  churn session rather than synthetic `0x3E` failures.
- Node 31's 71 onboarding "connecting..." attempts this run (vs. single
  digits for most other nodes, next-highest were 42/41 at 28/27) is a
  directly observable symptom behind its PDR collapse — it wasn't just
  losing individual packets, it was repeatedly failing to stay onboarded at
  all, which tracks with a genuinely marginal link rather than scattered
  packet loss on an otherwise-fine connection.
