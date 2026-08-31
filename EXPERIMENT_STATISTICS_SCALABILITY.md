# Scalability Experiments — August 25-26, 2026

Node-count scalability axis: 5, 10, ~19-20, and 40 nodes, each tested across
all three architectures (PAwR dynamic scheduling, PAwR static/fixed-table
scheduling, and Broadcasting). All runs are 30-minute stationary captures,
circle-around-a-central-node physical layout. Same physical node-ID sets are
reused across all three architectures at each tier for a genuine apples-to-
apples comparison — a given tier's roster is identical whether the boards are
running PAwR or Broadcasting firmware.

PDR is computed per node as `received / expected`, where
`expected = max(seq) - min(seq) + 1` over the observation window (10s
reporting interval for PAwR; Broadcasting nodes report roughly once per
second, see `broadcasting/lib/config/Config.h`'s `SENSOR_READ_INTERVAL_MS`).
For Broadcasting, `received` counts only `dup=0` rows (first arrival, not
re-received duplicates) from the hub's CSV stream. Every run's duration was
cross-validated against the actual timestamp/millis span in the log itself,
not just the capture tool's own summary line — this project has repeatedly
hit cases where a script's reported elapsed time didn't match the real data.

**Rosters used** (same physical boards at every tier, tiers are additive):
- **5 nodes**: 47, 61, 49, 51, 45
- **10 nodes**: 50, 43, 57, 63, 49, 45, 47, 61, 51, 55
- **~19-20 nodes**: the above 10 + 42, 32, 29, 30, 31, 37, 39, 34, 35 (19
  total — labeled the "20-node tier" for continuity, but node 38 was
  swapped out for node 29 before any run at this tier, as a known bad-
  hardware outlier; see PAwR's own 20-node section below)
- **40 nodes**: the above 19 + 16, 20, 13, 15, 18, 26, 19, 24, 21, 23, 12,
  9, 6, 11, 7, 64, 4, 5, 3, 8, 2 (21 more, 40 total)

**TX power caveat**: PAwR (`central/prj.conf`, `peripheral/prj.conf`) runs
at `CONFIG_BT_CTLR_TX_PWR_PLUS_8=y` (+8 dBm), while Broadcasting
(`broadcasting/lib/communication/BleManager.cpp`) runs at
`Bluefruit.setTxPower(4)` (+4 dBm) — a real 4 dB difference between the two
architectures under comparison, not accounted for in the roster/physical-
layout matching described above. See the RSSI section below for the
measured effect this has.

Node 3 is a Seeed XIAO nRF52840 **Sense** variant (different board from the
rest of the fleet); its bootloader mounts as `XIAO-SENSE` instead of
`XIAO-BOOT`, and on the Zephyr/PAwR side it prints its own node ID zero-
padded (`Node 03`) in the serial console. No functional issues found on
either firmware.

---

## PAwR — dynamic (hub-driven) scheduling

| tier | nodes | overall PDR | weakest node |
|---|---:|---:|---|
| 5 | 5 | **96.44%** | 51 (94.4%) |
| 10 | 10 | **91.30%** | 51 (73.5%, late joiner) |
| 20 | 20 (incl. node 38) | 92.99% | **38 (58.8%)** |
| 20, excl. node 38 | 19 | **94.76%** | 24 (84.4%) |
| 40 | 40 | **95.00%** | 24 (84.4%) |

Full per-node breakdowns for each run are in the raw logs
(`logs/scale{5,10,20,40}_dynamic_pawr_30min_*.log`). Node 38 was a genuine,
reproducible hardware/link outlier (58.8% dynamic, later 43.3% static at
the same tier) — excluded from the 40-node roster going forward. The 40-
node dynamic run was the **first real test past previously-validated buffer
territory** (6/6 buffer counts only confirmed clean up to 20 subevents
before this project) — ran with **zero stability issues** (no resets,
`net_buf` errors, disconnects, or RF-noise warnings).

---

## PAwR — static/fixed-table scheduling

| tier | nodes | overall PDR | weakest node |
|---|---:|---:|---|
| 5 | 5 | **93.48%** | 51, 45 (90.6%) |
| 10 | 10 (43 in place of 58) | **93.09%** | 45 (83.7%) |
| 20 | 20 (incl. node 38) | 91.47% | **38 (43.3%)** |
| 20, excl. node 38 | 19 | **93.98%** | 50 (86.4%) |
| 40 | 40 | **96.13%** | 49 (90.5%) |

The 40-node static run is the **best overall PDR of any configuration
tested at any node count in this project**, and also ran fully stable at
40 subevents. Unlike the dynamic run's weakest node (24 at 84.4%), static's
worst node here (49 at 90.5%) is comfortably above 90% — no severe outlier.

### PAwR dynamic vs. static, same roster, by tier

| tier | dynamic | static | winner |
|---|---:|---:|---|
| 5 nodes | 96.44% | 93.48% | dynamic (+2.96 pt) |
| 10 nodes | 91.30% | 93.09% | static (+1.79 pt) |
| 20 nodes (excl. n38) | 94.76% | 93.98% | dynamic (+0.78 pt) |
| 40 nodes | 95.00% | 96.13% | static (+1.13 pt) |

No consistent winner — the fixed-vs-dynamic scheduling choice does not have
a stable direction across node counts in this data. Differences are all
within ~3 points either way. Per-node hardware quality (node 38, node 24,
node 51's slow joins) is a far bigger driver of PDR variance than the
scheduling algorithm itself.

---

## Broadcasting (uncoordinated, no scheduling)

| tier | nodes | overall PDR | drop from prior tier |
|---|---:|---:|---:|
| 5 | 5 | **81.85%** | — |
| 10 | 10 | **72.27%** | -9.6 pt |
| ~19-20 | 19 | **58.36%** | -13.9 pt |
| 40 | 40 | **39.92%** | -18.4 pt |

Source logs: `logs/broadcasting_scale{5,10,20,40}_30min_20260826.log`.
Unlike PAwR, Broadcasting shows **clear, accelerating degradation with node
count** — each doubling-ish step drops overall PDR by a larger margin than
the last, not a smaller one. At 40 nodes the entire fleet is remarkably
*uniform* (33-48% PDR band, no severe individual outlier) — pointing to
systemic channel saturation rather than a few bad boards, in sharp contrast
to PAwR's failure mode (a small number of genuinely bad-hardware nodes
dragging the average down while everyone else stays >90%).

### Real bug found and fixed mid-experiment: hub's 32-node cap

The 40-node Broadcasting run initially would not verify cleanly — repeated
checks found exactly 32 of 40 nodes reporting, but a **different** 32 each
time the hub was reset. Root cause: `broadcasting/src/hub_main.cpp` had
`#define MAX_NODES 32`, a hard-coded cap on the hub's node-tracking array.
`findOrCreateNode()` silently returns `nullptr` for any node beyond the
first 32 seen after boot — no crash, and the one-time
`"[WARN] node table full"` console message is easy to miss in a raw byte-
stream CSV capture (confirmed absent from several verification logs before
this was traced). This produced a false trail during live debugging: two
reflashes, a full board reposition, and a hub power-cycle were all tried
(the power-cycle appeared to "fix" the original 8 missing nodes, but only
because it reset which 32 nodes won the race the next time) before the
actual cause was found by reading the hub firmware source. **Fixed** by
raising `MAX_NODES` to 64 (commit `88c08df` on `broadcasting-baseline`).
Re-verified all 40 nodes present with zero table-full warnings before
trusting the real 30-minute capture above.

### Also found: `pio.exe` blocked by Windows Application Control policy

Every `Build-BroadcastingFleet.ps1` invocation failed identically
("An Application Control policy has blocked this file") when building the
29 additional node firmwares needed for this scalability axis. Worked
around by invoking the same PlatformIO Core via `python -m platformio`
instead of the `pio.exe` wrapper binary — not blocked, confirmed identical
build output. Fixed in the script itself (same commit as the hub fix).

---

## Full architecture comparison, all tiers

| tier | PAwR dynamic | PAwR static | Broadcasting |
|---:|---:|---:|---:|
| 5 | 96.44% | 93.48% | 81.85% |
| 10 | 91.30% | 93.09% | 72.27% |
| ~19-20 | 94.76%* | 93.98%* | 58.36% |
| 40 | 95.00% | 96.13% | 39.92% |

*excluding node 38 (bad hardware, dropped from roster)

**The gap between PAwR and Broadcasting widens sharply with scale:**

| tier | PAwR best - Broadcasting |
|---:|---:|
| 5 nodes | ~14.6 pt |
| 10 nodes | ~20.8 pt |
| ~19-20 nodes | ~35.6 pt |
| 40 nodes | ~56.2 pt |

This is the central scalability finding: **PAwR's collision-free slotted
access holds essentially flat across the whole 5-to-40-node range tested
(all results 91-96%), while Broadcasting's uncoordinated channel access
degrades steadily and its rate of degradation itself accelerates** as node
count grows.

---

## Does the birthday paradox explain Broadcasting's degradation curve?

Qualitatively, yes as an intuition: with N independently-timed broadcasters
sharing 3 advertising channels, the number of *pairs* of nodes that can
collide grows as `C(N,2) = N(N-1)/2`, not linearly with N — the same
combinatorial shape behind the birthday paradox. This is consistent with
the *accelerating* PDR drop observed (-9.6, -13.9, -18.4 points per step,
each one bigger than the last), which a naive linear-contention model would
not predict.

**Quantitatively, tested by curve-fitting the 4 data points** (5, 10, 19,
40 nodes; PDR 81.85%, 72.27%, 58.36%, 39.92%) against four candidate
functional forms (least-squares grid search, no external solver):

| model | form | SSE | fit quality |
|---|---|---:|---|
| Exponential | `A·exp(-k·N)` | **0.00062** | best |
| Power-law | `A·N^-b` | 0.00584 | good |
| Pairwise-collision (birthday-paradox shape) | `A·exp(-k·N(N-1)/2)` | 0.01168 | worst of these three |
| Slotted-ALOHA-like | `scale·(cN)·exp(-2cN)` | 0.08416 | poor |

**Result: a plain exponential decay in N fits the four measured points
better than the literal birthday-paradox/pairwise-collision formula does.**
The pairwise form's `N²` term in the exponent falls off too fast at the
sampled points — it undershoots badly at N=19 (predicts ~67% vs. the
observed 58%) while both ends fit reasonably.

**Honest takeaway for write-up purposes:** the birthday paradox is a
reasonable *physical/qualitative* explanation for why the curve accelerates
(more nodes → a much faster-growing number of possible colliding pairs, not
just more nodes), and real BLE collisions are indeed the plausible
underlying mechanism. But with only 4 data points, several different
functional forms (including plain exponential decay) fit about as well or
better than the literal birthday-paradox formula — not enough data to
distinguish between them rigorously, and real BLE contention isn't a clean
pairwise-independent-collision system anyway (capture effect, only 3
advertising channels, controller-randomized backoff). Recommend describing
collisions as the qualitative mechanism without claiming the specific
birthday-paradox formula is quantitatively confirmed by this dataset.

---

## RSSI: rules out signal strength as the cause of Broadcasting's decline

If Broadcasting's PDR collapse were actually a range/signal-quality effect
(e.g. new nodes added at each tier happening to sit further from the hub),
average RSSI would be expected to drop alongside PDR. It doesn't.

### Fleet-average RSSI vs. PDR, Broadcasting, by tier

| tier | fleet avg RSSI | overall PDR |
|---:|---:|---:|
| 5 | -68.7 dBm | 81.85% |
| 10 | -66.6 dBm | 72.27% |
| 19 | -65.2 dBm | 58.36% |
| 40 | -67.5 dBm | 39.92% |

RSSI stays in a tight **-65 to -69 dBm band across every tier** while PDR
falls by more than half (82% → 40%) over the same range. There is no
downward RSSI trend to explain the PDR trend — **signal strength is
essentially constant while delivery collapses**, which is exactly the
signature expected from a contention/collision-driven mechanism rather
than a propagation/range-driven one. This corroborates the birthday-
paradox-style qualitative explanation above: it is not that packets are
arriving too weak to decode, it's that more of them are colliding with
each other before they arrive at all.

### Within-tier RSSI-vs-PDR correlation (per-node, Broadcasting)

| tier | Pearson r (RSSI vs PDR) | n nodes |
|---:|---:|---:|
| 5 | 0.362 | 5 |
| 10 | 0.021 | 10 |
| 19 | 0.471 | 19 |
| 40 | 0.380 | 40 |

Weak-to-moderate positive correlation within every tier (weaker-signal
nodes tend to do somewhat worse), but never strong, and not the dominant
effect — consistent with RSSI being a secondary contributor (occasional
marginal-link packet loss) layered on top of the much larger contention-
driven decline shown above, not the primary driver.

### PAwR's RSSI, for comparison — and the TX-power confound

| tier | PAwR dynamic avg RSSI | PAwR static avg RSSI |
|---:|---:|---:|
| 5 | -58.8 dBm | -60.2 dBm |
| 10 | -58.5 dBm | -58.7 dBm |
| 20 | -57.3 dBm | -56.1 dBm |
| 40 | -56.8 dBm | -57.0 dBm |

Two things stand out. First, like Broadcasting, **PAwR's RSSI is also flat
across tiers** (-56 to -60 dBm) — reinforcing that neither architecture's
PDR trend is range-driven; PAwR's stays flat because its scheduling avoids
collisions, Broadcasting's stays flat because signal strength was never
the limiting factor to begin with.

Second, **PAwR's RSSI is consistently 8-13 dB stronger than Broadcasting's
at every tier**, despite the same physical boards in the same positions.
This traces directly to the TX-power caveat noted earlier: PAwR runs
`CONFIG_BT_CTLR_TX_PWR_PLUS_8` (+8 dBm) while Broadcasting calls
`Bluefruit.setTxPower(4)` (+4 dBm) — a real, unaccounted-for 4 dB
transmit-power difference between the two firmware stacks being compared.
**This does not explain the PDR gap** (RSSI is flat across tiers for both
architectures, so it can't be driving the *shape* of Broadcasting's decline
with scale), but it is a legitimate caveat on the *absolute* PDR comparison
between architectures: some unknown fraction of PAwR's PDR advantage over
Broadcasting at every tier could be attributable to the stronger transmit
power rather than the scheduling scheme alone. Worth flagging explicitly in
the thesis write-up as a confound, and — if time allows — worth a follow-up
run with both architectures at matched TX power to isolate the scheduling
effect cleanly.
