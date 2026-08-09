# PAST sync failure investigation -- 2026-08-09

## The problem

On `redundant-slots-experiment` (34 subevents = 17 nodes x primary+backup,
`NUM_PRIMARY_SLOTS=17`), peripherals intermittently/consistently fail to
complete Periodic Advertising Sync Transfer (PAST) with central. Symptom on
the peripheral's own console, repeating forever:

```
Waiting for periodic sync...
[SENSORS] node NN seq K temp=...C humidity=...% flags=0x00
Timed out while synchronizing
Waiting for periodic sync...
```

The peripheral never once completes sync -- no `>>> Node NN (subevent ...)`
line ever appears on central's console for the affected node, and zero rows
land in `gui/sensor_data.db` for it. This is not a slow/marginal sync -- it
fails within seconds, every single attempt, for as long as it's been observed
running (5-10+ minutes in each test).

Central's own console shows nothing unusual: BLE init succeeds, "Start
Periodic Advertising" / "Start Extended Advertising" / "Scanning successfully
started" all print normally. A separate, apparently-benign USB-CDC boot-noise
sequence (`udc_nrf: Reset` / `udc: Failed to allocate net_buf 4095, ep 0x80`,
a handful of times right after boot) happens on every boot regardless of
whether the run later succeeds or fails -- confirmed not to be the direct
cause (see "Ruled out" below), though its role is not fully understood either.

## Timeline of what was tried and observed today

1. **10:02-10:16** -- Central + node 40, exact `aba6a56`/`3eaad70` commit
   (0dBm, no Coded PHY, 34 subevents), flashed fresh. Worked perfectly: 27+
   readings over 5+ minutes, zero gaps, zero errors.
2. Layered in this session's in-progress changes (Coded-PHY Kconfig
   refactor, various diagnostic tweaks -- PAST timeout 30s->60s, SDC
   periodic-adv event-length budget 7.5ms->1.5s, central-side radio-startup
   staggering 0->50ms->500ms, TX power 0dBm->+8dBm->+4dBm->0dBm). **Every
   one of these, individually and in combination, either made no
   difference or coincided with continued failure.**
3. Reverted every diagnostic change back to what was believed to be the
   original working state. Still failed.
4. Went back to the *literal* `3eaad70` commit build (rebuilt from a git
   worktree, not hand-reconstructed) -- **worked again**, cleanly, on the
   first console log captured.
5. Re-derived the working tree's actual diff against that commit: turned
   out PAST timeout and event-length overrides were still left in from
   step 2 (not actually fully reverted as believed in step 3). Reverted
   those for real, plus removed central's radio-startup staggering
   entirely, rebuilt. **Still failed.** (This was the point where it
   became clear "should be equivalent" reasoning wasn't reliable enough --
   needed to go back to literal, verified commits instead of
   reconstructed diffs.)
6. Reflashed the literal `3eaad70` build again (same one from step 4).
   **Worked again** (confirmed via console log: "Node 40 (subevent 22)...
   seq=1", "seq=2 [DUP: backup slot]").
7. Compared the literal-working build against the hand-reconstructed
   "should be equivalent" build from step 5: `.config` diff was one no-op
   unset line, but **binary diff was real** -- reconstructed build's
   `main()` stack frame was 120 bytes vs the working build's 88 bytes (see
   "Root cause found and fixed" below).
8. Applied ONLY the Coded-PHY Kconfig refactor (the one real source change,
   isolated) on top of the otherwise-literal-working state, rebuilt,
   flashed. **Reproduced the failure**, single-change, clean isolation.
9. Fixed the refactor to use preprocessor `#if IS_ENABLED(...)` instead of
   runtime `IS_ENABLED()`, so the Coded-PHY-off path (current default)
   generates byte-identical code to the pre-refactor version. Verified via
   `arm-zephyr-eabi-size`/`objdump`: `text` size and `main()`'s stack frame
   (`sub sp, #88`) now match the working baseline exactly.
10. Rebuilt, flashed. **MD5 of the resulting `zephyr.uf2` is byte-identical**
    to the literal `3eaad70` baseline `zephyr.uf2` used successfully in
    steps 4 and 6. **Flashed this identical binary to central -- failed.**
11. Swapped node 40's peripheral board for a genuinely different physical
    board (flashed as node 41, different `CONFIG_APP_NODE_ID`, same
    fixed/verified firmware). Central unchanged (still the byte-identical
    binary from step 10). **Also failed.**

## What this rules out

- **TX power** (0dBm, +4dBm, +8dBm all tested at 34 subevents): not the
  variable -- 0dBm fails just as reliably as elevated power once the
  underlying flakiness is present, and elevated power was confirmed to work
  fine in earlier, unrelated tests (34 subevents was soak-tested at 0dBm
  and separately 17 subevents was soak-tested at +8dBm, both clean).
- **PAST subscribe timeout** (`PAWR_PAST_TIMEOUT_UNITS`, 30s vs 60s): no
  effect. Failure happens well within 30s regardless, so this was never a
  "needs more time" problem.
- **SDC periodic-adv event-length scheduling budget**
  (`CONFIG_BT_CTLR_SDC_PERIODIC_ADV_EVENT_LEN_DEFAULT`, 7.5ms default vs
  1.5s explicit override): no effect.
- **Central-side radio-startup ordering/timing** (staggering
  `bt_le_per_adv_start`/`bt_le_ext_adv_start`/`bt_le_scan_start` with 0ms,
  50ms, or 500ms gaps): no effect. Confirmed both the crash-like
  "central goes silent after boot" symptom AND the "central boots fine,
  peripheral just never syncs" symptom happen with or without staggering.
- **The Coded-PHY runtime-`IS_ENABLED()` refactor's extra stack usage**:
  WAS confirmed as a real, reproducible cause in one specific isolated test
  (step 8) -- but fixing it (step 9-10, verified byte-identical binary to
  the known-good build) did NOT resolve the problem when retested (step
  10). So this was *a* real bug, worth keeping fixed on principle (see
  below), but evidently not *the* (only) cause of today's failures, or the
  failure has a second, still-unidentified trigger that coincidentally
  correlated with testing that refactor.
- **Node 40's specific physical board**: a genuinely different board
  (flashed as node 41) failed identically against the same central, so
  this is not specific to one peripheral board.
- **Firmware content, full stop**: step 10 used an MD5-verified, byte-for-byte
  identical `zephyr.uf2` to a build that worked cleanly twice (steps 4, 6)
  on the exact same central board. Flashing that identical binary again
  later failed. **Identical firmware produced different outcomes on
  different occasions.**

## What this leaves unexplained

Given identical firmware on central produced both a 5+ minute clean run and
multiple immediate hard failures, and swapping the peripheral board didn't
change the outcome, the remaining candidate explanations are:

- **Central's physical board/hardware state** -- never isolated the same
  rigorous way (swap-test) that node 40 was. Central has been reflashed
  and reset far more times than any single peripheral today (15+ times).
  Worth testing with a spare central board if one is available.
- **RF environment** -- something about interference, distance, or
  orientation that changed between the successful morning runs and now.
  Not actively investigated (no controlled distance/placement test done
  today).
- **A stateful SDC/controller condition that survives a plain UF2 reflash**
  -- this project has no `CONFIG_BT_SETTINGS`/bonding persistence in the
  application layer, so there's no obvious *application*-level stored
  state to blame, and a full power-off power-cycle of both boards was
  tried today without resolving it (see below) -- but this doesn't fully
  rule out something at the SoftDevice Controller / hardware level (e.g.
  radio calibration, flash wear, or a Nordic bootloader-level state) that
  neither a UF2 flash nor a power cycle clears.
- **Not actually random/environmental at all -- possibly intermittent**,
  i.e. this may come and go on a timescale not yet characterized (the
  morning run was 5+ minutes clean; today's failures were each tested for
  3.5-6 minutes before giving up). It's possible some runs would have
  eventually synced if left longer, though the peripheral-side timeout
  loop retrying every ~10s with zero successes across dozens of attempts
  in a single test window argues against "just needs more retries."

A full power-off power cycle of BOTH boards (not just a UF2 reflash / soft
reset) was tried once today and did NOT resolve the issue.

## Root cause found and fixed (real, but evidently not sufficient on its own)

**Bug**: `central/src/main.c`'s Coded-PHY support (added while porting from
`coded-phy-experiment`) used a runtime `IS_ENABLED(CONFIG_APP_USE_CODED_PHY)`
check to conditionally OR `BT_LE_ADV_OPT_CODED`/`BT_LE_SCAN_OPT_CODED` into
the advertising/scan parameters. To do this, it *always* allocated a local
`struct bt_le_adv_param pawr_adv_param` / `struct bt_le_scan_param
onboard_scan_param`, copied the const `BT_LE_EXT_ADV_NCONN` /
`BT_LE_SCAN_PASSIVE_CONTINUOUS` macro's value into it, and passed a pointer
to the local copy -- even when `CONFIG_APP_USE_CODED_PHY` is off (today's
default). Confirmed via disassembly (`arm-zephyr-eabi-objdump`) that this
added a real 32-byte stack frame increase in `main()` (`sub sp, #120` vs the
original `sub sp, #88`) and an extra struct-copy step at the exact
`bt_le_ext_adv_create()` / `bt_le_scan_start()` call sites. In one isolated
A/B test (this refactor alone, nothing else changed), this reproduced the
PAST sync failure 100% of the time; reverting just this one change restored
success.

**Fix applied**: converted the runtime `IS_ENABLED()` checks to preprocessor
`#if IS_ENABLED(CONFIG_APP_USE_CODED_PHY)` in both `central/src/main.c` and
`peripheral/src/main.c`, so the `CONFIG_APP_USE_CODED_PHY=n` path compiles to
the exact original direct-pointer-to-const-macro form (zero extra stack,
zero extra copy), and the struct-copy-and-OR path only exists in the binary
at all when Coded PHY is actually enabled. Verified byte-identical `.elf`
`text` size and `main()` stack frame size to the pre-refactor working
baseline; verified byte-identical `zephyr.uf2` MD5 to a build that worked
successfully twice.

**Status**: this fix is real and worth keeping (it removes a genuine,
reproduced-once source of extra boot-time resource pressure for zero
benefit when Coded PHY is off), but retesting after applying it still hit
the same failure, and a different peripheral board also failed against the
same central. So either this bug has a narrow reproduction window that
happened to align with one specific test today, or there is a second,
still-unidentified failure mode being conflated with it. Not safe to
conclude this fix alone solves the observed problem.

## Code differences currently in the working tree (`redundant-slots-experiment`, all uncommitted)

Relative to the last real commit (`3eaad70`, itself code-identical to
`aba6a56`):

- **`central/Kconfig`, `peripheral/Kconfig`**: new `CONFIG_APP_USE_CODED_PHY`
  bool option (default n), `select`s `BT_CTLR_PHY_CODED` only when enabled.
  Replaces an earlier, separately-fixed bug (unconditional
  `CONFIG_BT_CTLR_PHY_CODED=y` in `prj.conf` regardless of the toggle --
  see git history / earlier NOTES.md entries for that fix, unrelated to
  today's issue).
- **`central/prj.conf`, `peripheral/prj.conf`**: `CONFIG_BT_CTLR_PHY_CODED`
  no longer set unconditionally (relies on the Kconfig `select` above).
  `CONFIG_BT_CTLR_TX_PWR_PLUS_8`/`_PLUS_4` currently commented out --
  power is at 0dBm (default), matching the only state confirmed not to
  introduce a *separate* variable while this investigation is open.
- **`common/pawr_protocol.h`**: `PAWR_PAST_TIMEOUT_UNITS` back to 3000 (30s,
  original value). No other behavioral change; extensive comments added
  documenting the Coded-PHY Kconfig history and today's dead ends.
- **`central/src/main.c`**: the `#if IS_ENABLED(CONFIG_APP_USE_CODED_PHY)`
  fix described above, at the `bt_le_ext_adv_create()` and
  `bt_le_scan_start()` call sites. No staggering/sleep calls (removed).
- **`peripheral/src/main.c`**: same `#if`-gating fix applied to
  `conn_adv_param`/`bt_le_ext_adv_create()`. Structurally still uses
  `bt_le_ext_adv_create()`+`bt_le_ext_adv_start()` (extended-advertising
  API) instead of the original single-call `bt_le_adv_start()`, because
  that structural change is required infrastructure for Coded PHY to work
  at all (legacy `bt_le_adv_start()` cannot combine with
  `BT_LE_ADV_OPT_EXT_ADV` per its own Zephyr doc comment) -- this part
  was NOT reverted since removing it would mean re-doing real work to add
  Coded PHY back later, and it wasn't implicated by the isolated A/B test
  (which only changed central's code).

## Suggested next steps (not yet done)

1. Test with a spare/different **central** board (not just peripheral) to
   rule out central-side hardware state, mirroring the peripheral swap
   test already done.
2. If a second central isn't available, try a longer cool-down (e.g. leave
   both boards fully unpowered for several minutes to an hour, not just a
   quick unplug/replug) before the next test, in case there's a slow
   capacitor-discharge or thermal factor a quick power cycle doesn't clear.
3. Consider capturing a BLE sniffer trace (e.g. nRF Sniffer for Bluetooth
   LE) during a failing run to see whether central's periodic-adv train is
   actually malformed/absent over the air, versus the peripheral's
   PAST-triggered sync attempt itself failing for a receiver-side reason --
   this would definitively separate "central problem" from "peripheral
   problem" instead of inferring it from console logs alone.
4. Re-run the exact working `3eaad70` binary (already built, MD5 known) one
   more time as a fresh control, immediately before any further changes --
   if IT also now fails, that's strong confirmation this is purely
   environmental/hardware-state and not connected to any code path at all.
