# Architecture Comparison — Key Tables

Distilled from `EXPERIMENT_SUMMARY_20260905.md`, `EXPERIMENT_STATISTICS.md`,
and the (git-history-only, see note at bottom) `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md` /
`BROADCASTING_SITTING_SUMMARY.md` / `BROADCASTING_RESULTS_SUMMARY.md`. PDR =
unique seq received / expected span per node; RSSI = mean dBm across
received packets. Body positions are each rig's saved mapping in
`gui/body_mapping.json` (person_3 for nodes 30-42, person_4 for nodes
43-63) — real per-subject placement, not an assumed default.

All 13 raw logs behind every number in this doc are collected in
`logs/architecture_comparison_source_logs/` (originals also remain in
their usual `logs/`/`logs/dumps/` locations) — see that folder's `README.md`
for the per-file architecture/posture/hub-position/roster breakdown.

## 1. Sitting — Dynamic PAwR vs. Static PAwR vs. Broadcasting

Dynamic/static PAwR: person_3 roster (30-42). Broadcasting: person_4
roster (43-63) — different physical nodes, same 11 body positions.

| Architecture | Avg PDR | Avg RSSI | Runs |
|---|---|---|---|
| Dynamic PAwR | 99.6% | -63.0 dBm | 1 (0 disconnects) |
| Static PAwR | 95.0% | -62.3 dBm | 1 (5 disconnects, all benign — see note) |
| Broadcasting | 70.6% | -76.5 dBm | 2 (no disconnects — connectionless) |

Sitting is close to a wash between dynamic and static PAwR (both near
ceiling); Broadcasting sits ~25-29 points lower on PDR and ~13 dB weaker on
RSSI even with zero mobility, since it has no redundant primary+backup slot
mechanism to fall back on.

**Note**: static PAwR's sitting number was recaptured 2026-09-09
(`sitting_static_pawr_serial_20260909.txt`, via `tools/Watch-SerialLog.ps1`
on COM220), replacing two earlier 2026-09-05 runs that are no longer used
here — each of those had a fault-affected node (run 1: node 34 dropout,
node 41 not yet reflashed; run 2: large dropouts on nodes 34/40/41 during a
noisier RF period) dominating its raw PDR. The 2026-09-09 run has all 11
nodes present with no large single-node dropout, so it replaces both rather
than averaging alongside them. Its 5 disconnects were 2x `0x08` and 3x
`0x16` (benign per this project's decoder ring — central's own view of a
normal intentional disconnect).

### Per-node detail

| Body position | Node (PAwR) | Dynamic PDR | Dynamic RSSI | Static PDR | Static RSSI | Node (Broadcasting) | Broadcasting PDR (run1/run2) | Broadcasting RSSI (run1/run2) |
|---|---|---|---|---|---|---|---|---|
| Shin & Calf | 30 | 100.0% | -67.9 | 91.6% | -77.2 | 58 | 63.5% / 78.8% | -83.6 / -82.5 |
| Head / Forehead | 31 | 100.0% | -68.0 | 96.3% | -66.2 | 61 | 74.2% / 72.4% | -76.3 / -80.8 |
| Left Hand | 32 | 100.0% | -51.8 | 96.3% | -52.1 | 43 | 72.5% / 73.9% | -69.8 / -73.4 |
| Left Upper Arm | 33 | 99.4% | -64.5 | 96.3% | -65.2 | 47 | 79.2% / 78.1% | -69.8 / -73.3 |
| Upper Back | 34 | 97.8% | -77.2 | 88.3% | -79.2 | 50 | 52.7% / 66.1% | -81.4 / -83.3 |
| Upper Chest | 35 | 100.0% | -53.7 | 96.3% | -42.0 | 55 | 73.4% / 77.8% | -66.7 / -74.2 |
| Abdomen | 37 | 100.0% | -50.9 | 96.3% | -39.3 | 51 | 73.4% / 73.3% | -63.5 / -75.3 |
| Left Forearm | 38 | 100.0% | -47.3 | 100.0% | -51.0 | 45 | 80.9% / 69.3% | -75.2 / -75.0 |
| Lower Back | 40 | 99.4% | -74.2 | 84.4% | -75.1 | 56 | 72.4% / 52.4% | -82.9 / -86.1 |
| Right Foot | 41 | 99.2% | -74.3 | 100.0% | -75.0 | 63 | 56.4% / 69.2% | -82.8 / -83.6 |
| Right Thigh | 42 | 100.0% | -63.4 | 99.4%* | -62.8 | 57 | 71.5% / 72.2% | -66.6 / -75.9 |

\* Node 42 re-onboarded once mid-run (seq reset); PDR is reboot/epoch-aware
(pre- and post-reboot segments computed separately and summed), not a raw
single-span calculation.

## 2. Walking, front pocket — Dynamic PAwR vs. Static PAwR vs. Broadcasting

Dynamic/static PAwR: person_3 roster (30-42), same physical nodes.
Broadcasting: person_4 roster (43-63) — different nodes, not a controlled
swap against the PAwR rows, included for the body-position comparison only.

| Architecture | Avg PDR | Avg RSSI | Node roster |
|---|---|---|---|
| Dynamic PAwR | 84.6% | -67.0 dBm | 30-42 (person_3) |
| Static PAwR | 83.6% | -66.5 dBm | 30-42 (person_3) |
| Broadcasting | 56.6% (69.0% excl. back-worn 50/56) | n/a (not recorded for this run) | 43-63 (person_4) |

### Per-node detail

| Body position | Node (PAwR) | Dynamic PDR | Dynamic RSSI | Static PDR | Static RSSI | Node (Broadcasting) | Broadcasting PDR |
|---|---|---|---|---|---|---|---|
| Shin & Calf | 30 | 94.1% | -75.9 | 98.9% | -73.5 | 58 | 63.1% |
| Head / Forehead | 31 | 100.0% | -66.9 | 100.0% | -70.1 | 61 | 50.0% |
| Left Hand | 32 | 100.0% | -63.7 | 100.0% | -60.2 | 43 | 78.4% |
| Left Upper Arm | 33 | 98.4% | -73.7 | 100.0% | -71.0 | 47 | 84.1% |
| Upper Back | 34 | 33.5%* | -78.1 | 18.4%* | -80.1 | 50 | **1.6%** |
| Upper Chest | 35 | 100.0% | -63.7 | 100.0% | -60.5 | 55 | 73.9% |
| Abdomen | 37 | 100.0% | -43.3 | 100.0% | -43.6 | 51 | 87.1% |
| Left Forearm | 38 | 99.5% | -62.5 | 100.0% | -57.8 | 45 | 62.2% |
| Lower Back | 40 | **6.9%** | -72.5 | **1.8%**† | -77.0 | 56 | **0.8%** |
| Right Foot | 41 | 98.9% | -74.5 | 100.0% | -73.4 | 63 | 42.9% |
| Right Thigh | 42 | 99.5% | -62.1 | 100.0% | -64.1 | 57 | 78.9% |

\* Node 34 rebooted mid-run in both PAwR runs (seq reset); scattered loss,
real intermittent trouble, not a clean crash.
† Node 40 received once at run start, once mid-run, then silent — alive
and transmitting the whole time (seq kept climbing) but not received.

**Every architecture's front-pocket run shadows the two torso-back
positions (Upper Back / Lower Back)** — clearest in Broadcasting (1.6%,
0.8%) and static PAwR (18.4%, 1.8%), still visible but partially masked by
redundant slots in dynamic PAwR (33.5%, 6.9%). Every other position stays
≥94% (PAwR) or in a 42-87% band (Broadcasting, no redundancy to hide
run-to-run variance).

## 3. Walking, back pocket — Dynamic PAwR vs. Static PAwR vs. Broadcasting

Same person_3 roster (30-42) for all rows this time — a genuine controlled
comparison, all three architectures on the same physical nodes/positions.

| Architecture | Avg PDR | Avg RSSI | Runs |
|---|---|---|---|
| Dynamic PAwR | 77.8% / 85.6% (2 runs) | -71.4 / -73.7 dBm | 2026-09-04, 2026-09-06 |
| Static PAwR | 93.6% | -67.9 dBm | 2026-09-03 (1 run, "butt pocket") |
| Broadcasting | 58.4% / 50.6% (2 runs) | -83.8 / -84.3 dBm | 2026-09-05, 2026-09-06 |

### Per-node detail

| Body position | Node | Dynamic PDR (09-04/09-06) | Dynamic RSSI (09-04/09-06) | Static PDR | Static RSSI | Broadcasting PDR (09-05/09-06) | Broadcasting RSSI (09-05/09-06) |
|---|---|---|---|---|---|---|---|
| Shin & Calf | 30 | 86.6% / 79.3% | -76.1 / -78.6 | 98.3% | -75.6 | 10.7% / 17.3% | -88.9 / -87.8 |
| Head / Forehead | 31 | 50.3% / **14.8%** | -80.7 / -82.0 | 68.0% | -77.1 | **3.0% / 1.3%** | -87.9 / -86.0 |
| Left Hand | 32 | 97.8% / 90.7% | -70.9 / -74.5 | 99.4% | -69.1 | 67.4% / 62.8% | -82.7 / -85.8 |
| Left Upper Arm | 33 | 99.5% / 86.9% | -72.0 / -76.3 | 100.0% | -63.7 | 64.6% / 67.1% | -85.1 / -84.5 |
| Upper Back | 34 | 100.0% / 100.0% | -65.8 / -70.2 | 100.0% | -63.4 | 54.4% / 75.7% | -83.7 / -80.2 |
| Upper Chest | 35 | 64.6% / 100.0% | -72.6 / -74.0 | 100.0% | -68.4 | 80.3% / 55.4% | -84.9 / -86.4 |
| Abdomen | 37 | 56.5% / 98.9% | -67.2 / -74.1 | 100.0% | -67.4 | 76.7% / 39.3% | -83.4 / -87.0 |
| Left Forearm | 38 | 98.9% / 95.6% | -65.8 / -67.0 | 100.0% | -60.2 | 82.3% / 84.6% | -75.4 / -77.6 |
| Lower Back | 40 | 100.0% / 94.5% | -62.5 / -73.8 | 100.0% | -58.7 | 74.5% / 83.0% | -84.7 / -79.9 |
| Right Foot | 41 | 97.3% / 85.7% | -73.9 / -73.3 | 100.0% | -71.5 | 70.7% / 40.2% | -83.3 / -85.8 |
| Right Thigh | 42 | **4.5% / 95.4%** | -78.0 / -66.6 | 63.7% | -72.0 | 57.7% / 29.9% | -81.6 / -85.7 |

Bold = worst-or-near-worst that run. **Head/Forehead (31) is the only
position that stays bad across every back-pocket run of every
architecture** (50.3%/14.8% dynamic, 68.0% static, 3.0%/1.3%
Broadcasting) — a real structural effect, not noise (see
`dynamic_pawr_body_shadowing` memory). Node 42 (Right Thigh) shows the
opposite pattern — wildly unstable run-to-run (4.5%→95.4% dynamic PAwR,
63.7% static, 57.7%→29.9% Broadcasting), illustrating why single-run
numbers for any position other than Head/Forehead shouldn't be read as
fixed.

## 4. Front pocket vs. back pocket, by architecture

| Architecture | Front-pocket PDR | Back-pocket PDR | Δ (back − front) | Front RSSI | Back RSSI |
|---|---|---|---|---|---|
| Dynamic PAwR | 84.6% | 77.8% / 85.6% (avg 81.7%) | -2.9 | -67.0 dBm | -71.4 / -73.7 dBm |
| Static PAwR | 83.6% | 93.6% | +10.0 | -66.5 dBm | -67.9 dBm |
| Broadcasting | 56.6%* | 58.4% / 50.6% (avg 54.5%) | -2.1* | n/a* | -83.8 / -84.3 dBm |

\* Broadcasting's front-pocket run used the person_4 roster
(`serial_20260902_184209.txt`), a different physical node set than its
back-pocket runs (person_3) — the Δ here is a rough directional read, not
an apples-to-apples same-node comparison the way the PAwR rows are. Table 5
below does the real front-vs-back comparison using Broadcasting's two
same-roster (person_3) back-pocket runs against each other instead, plus
the per-position flip that *is* comparable across rosters (same 11 body
positions either way).

### Per-node/per-position: which positions flip between front and back pocket

Dynamic and static PAwR use the same person_3 nodes in both columns
(directly comparable). Broadcasting's front-pocket column is person_4
(43-63); back-pocket is person_3 (30-42) — different nodes, same body
positions, so only the *position* comparison is valid there, not a
node-level one.

**PDR**

| Body position | Dynamic PAwR front→back | Static PAwR front→back | Broadcasting front→back (different nodes, same positions) |
|---|---|---|---|
| Shin & Calf | 94.1% → 86.6%/79.3% | 98.9% → 98.3% | 63.1% → 10.7%/17.3% |
| Head / Forehead | 100.0% → 50.3%/14.8% | 100.0% → 68.0% | 50.0% → 3.0%/1.3% |
| Left Hand | 100.0% → 97.8%/90.7% | 100.0% → 99.4% | 78.4% → 67.4%/62.8% |
| Left Upper Arm | 98.4% → 99.5%/86.9% | 100.0% → 100.0% | 84.1% → 64.6%/67.1% |
| Upper Back | 33.5% → 100.0%/100.0% | 18.4% → 100.0% | **1.6%** → 54.4%/75.7% |
| Upper Chest | 100.0% → 64.6%/100.0% | 100.0% → 100.0% | 73.9% → 80.3%/55.4% |
| Abdomen | 100.0% → 56.5%/98.9% | 100.0% → 100.0% | 87.1% → 76.7%/39.3% |
| Left Forearm | 99.5% → 98.9%/95.6% | 100.0% → 100.0% | 62.2% → 82.3%/84.6% |
| Lower Back | **6.9%** → 100.0%/94.5% | **1.8%** → 100.0% | **0.8%** → 74.5%/83.0% |
| Right Foot | 98.9% → 97.3%/85.7% | 100.0% → 100.0% | 42.9% → 70.7%/40.2% |
| Right Thigh | 99.5% → 4.5%/95.4% | 100.0% → 63.7% | 78.9% → 57.7%/29.9% |

**RSSI (dBm)**

| Body position | Dynamic PAwR front→back | Static PAwR front→back | Broadcasting front→back (different nodes, same positions) |
|---|---|---|---|
| Shin & Calf | -75.9 → -76.1/-78.6 | -73.5 → -75.6 | n/a → -88.9/-87.8 |
| Head / Forehead | -66.9 → -80.7/-82.0 | -70.1 → -77.1 | n/a → -87.9/-86.0 |
| Left Hand | -63.7 → -70.9/-74.5 | -60.2 → -69.1 | n/a → -82.7/-85.8 |
| Left Upper Arm | -73.7 → -72.0/-76.3 | -71.0 → -63.7 | n/a → -85.1/-84.5 |
| Upper Back | -78.1 → -65.8/-70.2 | -80.1 → -63.4 | n/a → -83.7/-80.2 |
| Upper Chest | -63.7 → -72.6/-74.0 | -60.5 → -68.4 | n/a → -84.9/-86.4 |
| Abdomen | -43.3 → -67.2/-74.1 | -43.6 → -67.4 | n/a → -83.4/-87.0 |
| Left Forearm | -62.5 → -65.8/-67.0 | -57.8 → -60.2 | n/a → -75.4/-77.6 |
| Lower Back | -72.5 → -62.5/-73.8 | -77.0 → -58.7 | n/a → -84.7/-79.9 |
| Right Foot | -74.5 → -73.9/-73.3 | -73.4 → -71.5 | n/a → -83.3/-85.8 |
| Right Thigh | -62.1 → -78.0/-66.6 | -64.1 → -72.0 | n/a → -81.6/-85.7 |

Broadcasting's front-pocket run (`serial_20260902_184209.txt`) didn't have
RSSI recorded per node in the source doc, so that column starts at n/a —
only the back-pocket side (person_3, both 09-05/09-06 runs) is available.
Note **RSSI doesn't track PDR position-for-position** — e.g. Upper
Back/Lower Back's RSSI barely moves or even worsens slightly front→back on
PAwR (Upper Back: -78.1→-65.8 better, but Lower Back on static: -77.0→-58.7
much better, while dynamic Right Thigh goes -62.1→-78.0 worse even as its
PDR average holds) — consistent with this project's repeated finding
elsewhere that PDR is collision/scheduling-driven as much as pure
link-margin-driven, so RSSI alone doesn't predict which position will fail.

**The direction of the aggregate-PDR effect is architecture-dependent, not
universal**: static PAwR does *better* in the back pocket (+10 pts),
dynamic PAwR is roughly a wash (-2.9 pts), and Broadcasting is worse on
its own two-run back-pocket average (-2.1 pts front vs. back-avg, though
each individual back-pocket run undershoots the front-pocket run by more).
What's consistent across **all three architectures** is *which body
positions flip*: Upper Back and Lower Back go from near-total failure
front-pocket to at-or-near ceiling back-pocket (clearest in Broadcasting:
1.6%/0.8% → 54-83%), while Head/Forehead goes the opposite direction,
degrading in the back pocket in every architecture. The aggregate number
is mostly a function of how well each architecture's redundancy (or lack
of it, for Broadcasting) absorbs whichever specific positions get shadowed
that run — not of front-vs-back being inherently better or worse.

---

**Note on sources**: `PAWR_STATIC_HUB_PLACEMENT_SUMMARY.md`,
`BROADCASTING_SITTING_SUMMARY.md`, and `BROADCASTING_RESULTS_SUMMARY.md`
are referenced throughout this project's docs but no longer exist in the
working tree — recovered here from git history (commit `39b97f6`). Worth
restoring them to the repo if they're still meant to be the source of
truth for those runs.
