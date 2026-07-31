# Collaboration Notes

Shared scratchpad for messages, questions, and suggestions between whoever is
working on `central/` and whoever is working on `peripheral/`. Newest entries
at the top. Sign your entries so it's clear who's asking/answering.

This file is pushed automatically by `tools/Sync-And-Build.ps1` alongside the
serial logs in `logs/`, so it'll show up on the other person's next `git
pull`/`fetch` without either of you needing to remember to push it by hand.

## 2026-07-31 — periodic sync bug: PAST event never reaches peripheral's controller

Findings from correlating `logs/central_20260731_105736.log` with fresh peripheral
captures (`logs/peripheral_20260731_111128.log`, and a debug-logging run at
`logs/peripheral_20260731_113602.log`):

1. **The central-side connection is fine.** Every cycle in the central log completes
   cleanly: `PAST sent` -> `Discovery started` -> `PAwR config written` ->
   `Disconnected, reason 0x16` (benign, self-initiated). No `0x08` CONN_TIMEOUT seen
   in that log at all.

2. **But there's a latent race in the disconnect timing** (not yet the main bug, but
   worth fixing anyway): central holds the connection open via
   `k_sleep(K_MSEC(per_adv_params.interval_max * 5 / 4))` before disconnecting
   ([central/src/main.c:514](central/src/main.c#L514)) -- that's ~10000ms. The
   connection's supervision timeout (`onboard_conn_param`,
   [central/src/main.c:258](central/src/main.c#L258)) is *also* 10000ms
   (`BT_GAP_MS_TO_CONN_TIMEOUT(10000)`). Those two ~10s windows are numerically
   equal, so any small radio jitter makes the supervision timeout fire first,
   producing `0x08` instead of a clean `0x13`/`0x16`. Confirmed in a fresh
   peripheral capture: 5 cycles, alternating clean (0x13) and 0x08 disconnects,
   both taking ~9.4-11.4s. Suggest widening the supervision timeout to give real
   margin over the hold time (e.g. 15-20s), or shortening the hold.

3. **The real bug: even a fully clean cycle never produces a periodic sync.** I
   rebuilt peripheral with `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=y` added to
   `peripheral/prj.conf` (already committed) to get full HCI-level tracing, and
   captured 5 complete onboarding cycles, including ones with a full clean ~10s
   hold + `0x13` disconnect. **Zero occurrences of anything PAST-related appear
   anywhere in that log** -- no `LE Periodic Advertising Sync Transfer Received`
   HCI event, success or failure. I checked Zephyr's handler for that event
   (`scan.c:1454-1458` in `C:\ncs\v3.3.0\zephyr\subsys\bluetooth\host\scan.c`) --
   it logs at `LOG_DBG` even on failure (`"PAST receive failed with status
   0x%02X"`), so this isn't a filtered-out failure, the event never fires at all.

   This rules out connection timing/radio contention as the root cause of the
   sync failure itself (fix #6 in Summary.md) -- a clean full-duration connection
   still doesn't produce a sync. Central believes `bt_le_per_adv_set_info_transfer()`
   succeeded (`PAST sent`, err 0), but the peripheral's controller never
   processes any sync transfer as a result.

**Request for whoever's on `central/`:** please add
`CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=y` to `central/prj.conf` too, rebuild/reflash,
and capture a log during a run where you can see what HCI command/status
central's own controller reports around the `bt_le_per_adv_set_info_transfer()`
call (should be logged as a `bt_hci_cmd_done` line near "PAST sent" in the app
log). If central's HCI command itself completes with status 0x00, that means
the failure is below the host (an actual over-the-air PAST PDU issue, or a
controller/firmware capability gap for this specific PAwR+PAST combination) and
we may need a BLE sniffer to go further. If central's command itself fails or
never completes, that narrows it to the send side directly. Push the resulting
log + this file back via `Sync-And-Build.ps1 -App central`.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — central's PAST HCI command completes with status 0x00

Did what you asked: added `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=y` to `central/prj.conf`
(matching what you already had on peripheral), rebuilt/reflashed, and captured a 90s
HCI-debug log — `logs/central_20260731_114524.log` (pushed, commit `13ab69b`).

**Answer to your question: central's PAST send succeeds at the host/controller
level, every time.** Right before each `PAST sent` print, the log shows:

```
hci_cmd_complete: opcode 0x205b
hci_cmd_done: opcode 0x205b status 0x00
```

`0x205b` is `LE Periodic Advertising Set Info Transfer`. Confirmed across all 3
onboarding cycles captured in that run — always `status 0x00`, no errors, no
retries.

So per your own branching in the previous entry: since central's HCI command
completes clean (`0x00`) but your peripheral-side capture shows **zero**
PAST-related HCI events ever arriving (not even a logged failure), the bug isn't
in either side's host/command handling — it's either the over-the-air PAST PDU
itself, or a controller-level reception/capability gap on the peripheral's
controller. Both host layers are clean, so I think you're right that this needs
a sniffer to go further.

**Also fixed while I was in `central/src/main.c`:** the supervision-timeout race
you flagged (item #2 in your entry above) — the onboarding connection's
supervision timeout was exactly equal to the ~10s post-PAST hold
(`BT_GAP_MS_TO_CONN_TIMEOUT(10000)` vs. `interval_max * 5/4` = 10000ms), so
ordinary jitter could make either one win the race. Widened the timeout to 18s
for real margin. Should reduce (not necessarily eliminate — still worth
watching) the stray `0x08` disconnects you saw alternating with clean `0x13`s.

**On the sniffer:** nRF52840 can absolutely do this — Nordic ships an official
`nRF Sniffer for Bluetooth LE` firmware that pairs with Wireshark and captures
raw link-layer PDUs, including periodic advertising/PAST traffic, which is
exactly the layer neither of our HCI logs can see. A phone's BLE scanner app
won't cut it (app-level only, no link-layer visibility), and even a phone's HCI
snoop log would just be host-level again — same layer we've already exhausted
on both sides. I've got a spare nRF52840 free to flash as a dedicated sniffer
(not reusing either the central or peripheral board, since it needs to sit
uninvolved and just listen) — I'll set that up next and see if I can catch the
actual PAST PDU (or its absence) over the air during a sync attempt.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — minimal-repro test mode added (parallel to the sniffer)

Since the sniffer is a bigger undertaking, added a cheap thing to try in the
meantime: a compile-time toggle that strips this project down close to
Nordic's original `periodic_adv_rsp` sample, to check whether the sync bug is
structural or tied to our larger interval/subevent count. Both are worth
running -- this doesn't replace the sniffer plan.

**What changed:**
- `common/pawr_protocol.h`: added `#define APP_MINIMAL_REPRO 1` at the top of
  the timing section. When set, `NUM_SUBEVENTS` drops from 20 to 5 and
  `PAWR_INTERVAL_UNITS` drops from 10s (`0x1F40`) to ~318.75ms (`0xFF`, the
  original sample's interval). Flip to `0` to restore full production timing.
- `central/src/main.c`: right after `PAST sent`, if `APP_MINIMAL_REPRO` is set,
  central now `goto disconnect`s immediately instead of doing GATT discovery +
  the slot-assignment write. So the whole onboarding shrinks to just
  connect -> PAST -> hold -> disconnect, no GATT round-trip in between.
- **No peripheral code change needed** for the fixed-slot part: peripheral's
  `pawr_timing` struct ([peripheral/src/main.c:40-44](peripheral/src/main.c#L40-L44))
  is a static struct with no initializer, so it's already zero-initialized to
  subevent 0 / response slot 0 by default -- exactly what we want once central
  stops writing to it.

**Why both changes together:** isolates purely the connect/PAST/sync pipeline
that's actually broken, with nothing else (GATT ops, larger interval/subevent
count) that could be masking or interacting with it. If this minimal version
still never syncs, that's strong evidence it's environment/board/controller-
specific rather than anything in our application code. If it *does* sync,
we bisect from here -- reintroduce GATT slot assignment first, then grow the
interval/subevent count back up, to find which change actually breaks it.

**Status:** I've got the peripheral board here, so I'm rebuilding/reflashing
it with this now. **Central needs the same rebuild+reflash on your end** for
this to be a valid joint test (mismatched timing between the two would just
produce noise, not signal) -- pull, rebuild `central`, reflash, and let's both
run at the same time. Remember this is a *toggle*, not a permanent design
change -- don't leave `APP_MINIMAL_REPRO` at 1 once we're done with it, and
don't relitigate the dynamic slot-assignment design over this (see Summary.md's
"design decisions already made" section -- this is a diagnostic detour, not a
redesign).

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — central rebuilt/reflashed with minimal-repro, ran the joint test

Done on my end: rebuilt + reflashed `central` with `APP_MINIMAL_REPRO=1` (already
set in the shared header, no code change needed beyond the rebuild), ran a 45s
capture — `logs/central_20260731_123023.log` (pushed).

Central's side looks correct: onboarding cycles now run every ~300-600ms instead
of every 10s (matches the ~319ms repro interval), each one doing `PAST sent` ->
immediate disconnect (no GATT step, as designed) -> rescan. That part of the
toggle is working as intended.

**I can't tell from central's log alone whether sync actually succeeded** --
"Periodic sync established" / sync failures are printed on the peripheral side,
not central's. Whoever has the peripheral capture from the same window (starting
~12:30:23, 45s long) should check that log for sync events and correlate against
`logs/central_20260731_123023.log`'s timestamps to see which specific PAST cycle(s)
lined up with any sync attempt on your end.

One thing I noticed but haven't chased down: there's an ~18s gap in central's
`PAST sent` cadence around `12:30:34.2` to `12:30:52.5` (one connection cycle
took much longer than the surrounding ones). Might be nothing at this log density
(HCI debug logging is extremely verbose at this interval -- this file is ~18k
lines for 45s), but flagging in case it lines up with something on your side.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — minimal-repro result: still zero syncs, over 85 tries

Ran a fresh 90s peripheral capture starting 12:33:44 (`logs/peripheral_20260731_123344.log`,
pushed) against your already-running central (didn't need to line up with your
exact 45s window -- central's `while(true)` loop keeps cycling on its own, so I
just captured a new window against whatever it was already doing).

**Result: 85 connect/disconnect cycles, all clean (`Disconnected, reason 0x13`,
none of the earlier `0x08`s -- consistent with your supervision-timeout fix
holding up), and zero occurrences of `Periodic sync established`, any
PAST-related HCI event, or subevent `0x18` anywhere in the log.** Checked with
a grep across the whole file, not just a sample.

So the minimal-repro experiment has a clear answer: **stripping out the GATT
slot-assignment dance and shrinking the timing down to match the original NCS
sample changes nothing.** Zero syncs in 5 cycles at full production timing,
zero syncs in 85 cycles at near-stock timing. That's a much bigger sample than
we had before, and it rules out both "GATT round-trip interferes with PAST"
and "the 10s/20-subevent scale is the problem" as explanations. Whatever's
wrong is orthogonal to both of the things this toggle changed.

Given that, I think the minimal-repro line of investigation is basically
exhausted -- probably not worth spending more time tuning `APP_MINIMAL_REPRO`
variants. The sniffer is the more promising path now; this at least confirms
it's not chasing a red herring.

Re: the ~18s gap you flagged -- I see the same pattern independently in my own
capture (gaps at `12:34:05.072`->`12:34:23.164` and `12:34:38.210`->`12:34:56.250`,
also ~18s). Same gap length showing up on both sides at different points in
time makes "just noise" less likely -- feels like it could be a controller-level
backoff/cooldown after some number of rapid connect/disconnect cycles, or a
scan-restart hiccup. Worth a look if the sniffer is up before this gets
revisited, since it'd catch whatever's happening on-air during one of those
gaps too. Not blocking anything, just flagging since we both independently
noticed it.

Remember to flip `APP_MINIMAL_REPRO` back to `0` in `common/pawr_protocol.h`
once we're done referencing these logs, so neither of us accidentally ships
the stripped-down timing.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — important: stock Zephyr sample synced successfully a few days ago

New information that changes the picture: the stock, completely unmodified
`zephyr/samples/bluetooth/periodic_adv_rsp` + `periodic_sync_rsp` pair was
tried on this same hardware a few days ago and **it worked** -- periodic sync
established successfully. That wasn't mentioned earlier in this thread because
it predates the current debugging session, but it's important: it means the
board/controller/SDK combination is *not* incapable of PAwR+PAST (rules out
the "controller feature gap" branch of our either/or from a few entries up).
It also means something specific changed or was never carried over correctly
in this project's adaptation, since even `APP_MINIMAL_REPRO` (which we thought
was close to stock) still isn't literally the stock sample -- it still carries
our device name, Kconfig options, connection-param overrides, sensor code
(compiled but unused), status LED, and (on peripheral) the HCI debug logging,
none of which exist in the real sample.

**Re-running the actual stock sample now to confirm it still works and get a
clean known-good baseline log to diff against.** I'm building
`C:\ncs\v3.3.0\zephyr\samples\bluetooth\periodic_sync_rsp` (unmodified,
straight from the SDK, board `xiao_ble/nrf52840`) and flashing it to the
peripheral board I have here. **Could you do the same with
`periodic_adv_rsp`** on the central board on your end? Both samples build
standalone (their own `prj.conf`/`CMakeLists.txt`, no dependency on anything
in this repo) -- just point `west build` at the sample directory instead of
`central/`/`peripheral/`. Once both are flashed, watch for `Periodic sync
established.` on the sync_rsp side.

If the stock pair still syncs today: we diff our project's central/peripheral
against the stock samples line-by-line to find what actually changed (my bet,
given what's ruled out so far, is something in the extended/periodic
advertising parameter values, the PAST subscribe parameters, or Kconfig -- not
the GATT/slot-assignment layer, since minimal-repro already showed that's not
it). If the stock pair *doesn't* sync anymore either: that points at something
that changed in the environment itself since a few days ago (SDK/toolchain
update, controller firmware, board damage) rather than in either app's code.

— Alejandro (session assisted by Claude), 2026-07-31

<!-- New entries go above this line -->
