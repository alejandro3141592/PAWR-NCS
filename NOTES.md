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

---

## 2026-07-31 — FOUND IT: peripheral/prj.conf was missing CONFIG_BT_PER_ADV_SYNC_TRANSFER_SENDER

Didn't even need to wait for a fully-stock central -- the stock
`periodic_sync_rsp` peripheral uses the same device name our project already
scans for (`"PAwR sync sample"`), so it connected straight to our own
already-running (minimal-repro) central. And it worked immediately: continuous
`Indication: subevent 0, responding in slot 0` for the full 60s capture
(`logs/stock_periodic_sync_rsp_20260731_124324.log`, pushed). That pins the bug
down to our peripheral's code/config specifically -- central, the boards, and
the environment are all fine.

Diffed our `peripheral/prj.conf` against the stock sample's line by line. Found
it:

```
Stock:                                    Ours (before):
CONFIG_BT_PER_ADV_SYNC_TRANSFER_SENDER=y  (missing)
CONFIG_BT_PER_ADV_SYNC_TRANSFER_RECEIVER=y CONFIG_BT_PER_ADV_SYNC_TRANSFER_RECEIVER=y
```

We had `RECEIVER` but not `SENDER`, even though the peripheral only logically
*receives* a sync transfer -- but Nordic's own sample enables both on that
side. No build error, no runtime error from omitting it, it just silently
leaves out whatever internal capability the combined flag pair enables in the
SoftDevice Controller -- which is exactly consistent with everything we saw:
central's HCI command reporting success while the peripheral's controller
never processed anything as a result.

**Added `CONFIG_BT_PER_ADV_SYNC_TRANSFER_SENDER=y` to `peripheral/prj.conf`,
rebuilt/reflashed** (still with `APP_MINIMAL_REPRO=1`, to keep matching your
currently-running central) and captured a fresh log
(`logs/peripheral_20260731_124738.log`, pushed). **It works**: 141 successful
`>>> Poll received: subevent 0, responding in slot 0` responses over the ~60s
capture (43 benign misses mixed in, consistent with the occasional drops we
always saw even in the working stock-sample baseline). This is our own
project's code, not the stock sample -- the fix is real.

(Checked `central/prj.conf` for symmetry: it already only has `SENDER`, no
`RECEIVER`, matching stock `periodic_adv_rsp` exactly -- central never needed
a change.)

**Next steps, in order:**
1. Flip `APP_MINIMAL_REPRO` back to `0` in `common/pawr_protocol.h` on both
   sides (full production timing: 20 subevents, 10s interval, GATT slot
   assignment restored) and do one more joint test to confirm the fix holds
   at full production scale, not just the stripped-down config.
2. Once confirmed, we can drop `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG` from both
   `prj.conf`s (diagnostic-only, very verbose) and stand down the sniffer plan
   -- shouldn't be needed anymore.
3. Update `Summary.md`'s "NOT YET RESOLVED" section -- this was the primary
   open bug, it's resolved now. (Also still owes that stale 100-150ms
   connection-interval correction from a few entries back.)
4. Worth someone filing/checking upstream whether this is a known NCS/Zephyr
   sample-vs-Kconfig-dependency gap, since it's a genuinely easy trap: nothing
   in the Kconfig `depends on` graph for `BT_PER_ADV_SYNC_TRANSFER_RECEIVER`
   requires `SENDER`, so there's no automated warning if you only enable what
   seems logically necessary.

Ready to flip the toggle back and retest whenever you are.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — new problem: full-scale (20 subevent / 10s) central hangs on boot, unrelated to the SENDER fix

Flipped `APP_MINIMAL_REPRO` back to `0` on central as requested (step 1) and hit
a new, separate issue -- central now hangs very early, before ever reaching a
connection attempt, so this isn't confirmed working at full scale yet.

**Symptom:** central boots fine through BLE init, prints `Start Periodic
Advertising` / `Start Extended Advertising` / `Scanning successfully started`,
then almost immediately:
```
<err> udc: Failed to allocate net_buf 4095, ep 0x80
<err> udc: Failed to allocate net_buf 4095, ep 0x80
```
repeating a few times, then **total silence** -- no more output at all, no PAST
attempt, nothing, for the rest of every capture window I tried (60s+). `udc` is
the USB device controller driver (the console's own USB-CDC connection).

**Ruled out so far:**
- Not a bad flash -- reproduced identically across a full pristine rebuild +
  reflash.
- Not USB hub/EMI -- reproduced with the hub fully disconnected and on a
  different physical port, away from other USB devices.
- Not `CONFIG_HEAP_MEM_POOL_SIZE` (it's `0`, but was already `0` in every
  earlier build today too, including ones that worked fine) -- not a new
  regression from that.
- Not a static-buffer sizing issue -- `bufs[NUM_SUBEVENTS]` /
  `backing_store[NUM_SUBEVENTS][PACKET_SIZE]` are compile-time static arrays,
  and the size difference between 5 and 20 subevents is a few hundred bytes,
  nowhere near enough to exhaust anything on its own.

**Important scope realization:** every single test that's worked today --
`APP_MINIMAL_REPRO`, and even the literal stock `periodic_adv_rsp`/
`periodic_sync_rsp` sample pair -- ran at the stock sample's own defaults
(`NUM_SUBEVENTS=5`, `interval_min/max=0xFF` / ~319ms). **This is the first time
all session that central has actually run at this project's real target scale
(20 subevents, 10s interval).** We've verified the SENDER fix at small scale,
but we have zero evidence yet that 20 subevents / 10s interval works on this
hardware+SDK at all -- that's new territory, not a regression of anything
previously validated.

Given the failure shows up right as periodic advertising ramps up to a much
higher subevent count than anything tested before, my leading guess is some
shared HCI/controller buffer pool getting starved by the 4x jump in subevent
data traffic, indirectly starving USB's own buffer allocation -- but I don't
have hard evidence for that yet, just the timing correlation and having ruled
out the alternatives above.

**Suggest next:** rather than jumping straight from 5 to 20 subevents, test an
intermediate step (e.g. 10 subevents, or keep 5 subevents but the full 10s
interval, tested separately) to isolate whether it's subevent *count* or
interval *length* that triggers this -- that'll narrow down which Kconfig
buffer-count option (`CONFIG_BT_BUF_*`, or something in the SDC's own
periodic-adv buffer sizing) actually needs raising. Also worth checking
Nordic's SDC release notes / Kconfig for anything explicitly capping subevent
count or periodic-adv buffer pool size by default.

Central is currently in this broken state on my end (build already committed,
`common/pawr_protocol.h` @ `APP_MINIMAL_REPRO=0`) -- don't assume it's usable
for testing against your peripheral until this is resolved.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — scale isolation attempt: mode 2 (20 subevents, short interval) produces zero console output at all

Added a temporary `APP_SCALE_TEST` knob to `common/pawr_protocol.h` (see comment
there) to separate the two variables the working/broken configs conflate:
subevent *count* vs. interval *length*. Tried mode 2 first: 20 subevents, kept
at the short ~319ms interval.

**Result so far is inconclusive but concerning: zero console output whatsoever**
-- not even the boot banner (`*** Booting nRF Connect SDK ***`) that mode 0
still managed to print before its `udc` errors. Tried, in order: capture
immediately after auto-flash, capture after a 5s settle delay, a single
manual reset, and re-flashing from a bootloader-mode-already-sitting board
with an immediate zero-delay capture -- all came back completely empty. Board
does leave bootloader mode and re-enumerate its COM port normally each time,
so it's not stuck in the bootloader; it's running *something*, just producing
no serial output at all, which is a step earlier/worse than mode 0 (which at
least got through the full boot sequence, BLE init, and
`Scanning successfully started` before hanging).

**Not confirmed yet whether this is:**
- A genuinely earlier failure than mode 0 (e.g. 20 subevents breaks something
  in the periodic-adv parameter setup itself, before advertising even starts,
  independent of interval) -- would mean subevent *count* alone is sufficient
  to break something, and it's worse than the mode-0 symptom, not equivalent.
- Something wrong with the `APP_SCALE_TEST` mode-2 branch itself (typo/bad
  interaction with `NUM_RSP_SLOTS`/`PAWR_SUBEVENT_INTERVAL`/etc. -- I set
  `PAWR_INTERVAL_UNITS=0xFF` but left `PAWR_SUBEVENT_INTERVAL=0x20` (40ms)
  unchanged; with 20 subevents at 40ms spacing each, that's 800ms of
  subevent train inside a ~319ms periodic interval, which is almost certainly
  an invalid/rejected parameter combination the stock sample's own math never
  needed to account for -- the stock sample only ever paired 5 subevents
  with that same 319ms interval. **This might just be an invalid config
  I created, not a real finding about the actual bug** -- flagging this
  as the most likely explanation before reading too much into "zero output."
- Still possibly something board/capture-tooling related, though the repeated
  zero-delay/manual-reset attempts make that less likely than for mode 0's
  case (where a manual reset had at least sometimes helped before).

**Haven't tried mode 3 yet** (5 subevents, full 10s interval -- isolates
interval length, keeps a parameter combination we know is individually valid
on each axis) -- doing that next, since mode 2 as configured may not be a
valid test at all given the subevent-train-vs-interval math above.

central is currently flashed with the mode-2 build (not usable for testing).
`common/pawr_protocol.h` has `APP_SCALE_TEST=2`.

**Update, same sitting: confirmed mode 2 was an invalid test, not a real
finding.** Did the math I'd flagged above as the likely explanation: 20
subevents * `PAWR_SUBEVENT_INTERVAL` (40ms) = an 800ms subevent train, which
literally cannot fit inside a ~319ms periodic interval. Mode 2 as I'd set it
up was self-contradictory parameters, not a valid isolation of subevent count
-- the zero-output result tells us nothing about the real bug, just that
malformed PAwR params fail even harder/earlier than the mode-0 symptom
(reasonable to expect, not news).

**Retiring mode 2, moving straight to mode 3** (5 subevents, 10s interval --
200ms subevent train, fits trivially in either interval, so it's a clean
single-variable change from the known-working mode 1). Updated the
`APP_SCALE_TEST` comment in `common/pawr_protocol.h` to record this so mode 2
doesn't get reused by mistake, and set `APP_SCALE_TEST=3` now. Testing that
next.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-07-31 — FOUND IT (part 2): mode 3 works cleanly, isolates the trigger to subevent COUNT, not interval

Ran mode 3 (5 subevents, full 10s interval) -- `logs/central_20260731_143509.log`
(pushed). **Clean result, no `udc` errors, no hang.** Full boot, scanning
started, and then it actually received live sensor data:

```
Scanning successfully started
>>> Node 01 (subevent 0): skin_temp=28.50C humidity=44.6% seq=673
>>> Node 01 (subevent 0): skin_temp=28.50C humidity=44.7% seq=703
>>> Node 01 (subevent 0): skin_temp=28.50C humidity=44.6% seq=734
```

Readings arriving every 10s, matching the interval exactly -- periodic sync,
PAST, and response-slot handling are all working end-to-end at this config
(bonus reconfirmation that the SENDER fix holds beyond the earlier
minimal-repro scale too, since this is the full 10s production interval, just
with fewer subevents).

**This isolates the USB hang trigger cleanly: it's subevent COUNT, not
interval length.** Mode 0 (20 subevents, 10s) hangs. Mode 3 (5 subevents, 10s)
works perfectly. Interval length is the same in both -- the only variable
that changed is 5 -> 20 subevents, so that's what's exhausting whatever buffer
pool `udc` is drawing from. Interval length is fully exonerated at this point.

**Next step:** narrow down where between 5 and 20 subevents the hang starts
(e.g. try 10, then binary-search from there) to find the actual threshold,
then look at which specific `CONFIG_BT_BUF_*` / SDC buffer-count Kconfig
option scales with subevent count and needs raising for 20 to work. Given
we need all 17 (+3 spare = 20) subevents for the real deployment, this isn't
optional -- something needs to give (more buffers, or fewer subevents than
20 with a different multiplexing scheme) before production scale is usable.

`common/pawr_protocol.h` currently has `APP_SCALE_TEST=3` (this working
config) -- fine to leave central here for now if anyone wants a working
reference point, just remember it's still a temporary diagnostic value, not
`0`.

— Alejandro (session assisted by Claude), 2026-07-31

---

## 2026-08-01 — mode 3 confirmed stable over a full 30-minute run

Before going further on the subevent-count threshold search, ran a proper
30-minute capture of central (still mode 3: 5 subevents, 10s interval) to
make sure the earlier 60s clean result wasn't just luck --
`logs/central_mode3_30min_20260801.log` (pushed), 10:13:05 to 10:43:04.

**Fully clean for the entire 30 minutes: zero `udc`/error lines, 159 readings
received, consistent ~10s cadence start to finish, no stalls.** Occasional
single-reading gaps in the sequence numbers (9->11, 13->15, etc.) throughout,
plus one slightly bigger one (167->174, missing 6) -- all consistent with the
normal miss rate we've seen before, nothing that looks like a new problem.
This is a solid confirmation, not just a lucky short window -- mode 3 is
genuinely stable.

Meant to overlap this with a simultaneous peripheral capture for direct
comparison, but only got the timing lined up on the third attempt (peripheral
wasn't actually recording yet on the first two tries) -- final central window
is 10:13:05-10:43:04. Only build logs have shown up from the peripheral side
so far (`peripheral_build_20260801_100159.log`), not an actual serial capture
in that window yet -- will compare once that's pushed.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — peripheral's matching 30-min capture is in, and it cross-validates yours precisely

Ran the peripheral side of this (`logs/peripheral_20260801_101223.log`, pushed,
10:12:23-10:42:23 -- overlaps your 10:13:05-10:43:04 window almost entirely).

**The gap you flagged (seq 167->174, ~70s, 6 missed readings) is a real event,
and peripheral's log shows exactly what caused it:**

```
[10:39:56.014] Disconnected, reason 0x13
[10:40:04.646] Connected, err 0x00
[10:40:14.595] Periodic sync established.
```

Central's last reading before the gap was `seq=167` at `10:39:14`, next was
`seq=174` at `10:40:24` -- lines up almost exactly with peripheral dropping
sync at ~10:39:56, reconnecting 8s later, and re-establishing sync 10s after
that. So this wasn't noise or a fluke in either log alone -- it's one real,
brief resync cycle that both sides independently recorded, and it self-healed
in about a minute with no intervention needed. Consistent with the existing
retry/reconnect design, not a new problem.

Aside from that one blip: 166 successful `Poll received` responses + 10
`Failed to receive` (normal miss rate) + 180 sensor reads over the 30 minutes
-- all consistent with your 159-readings-received count once you subtract the
~70s gap and account for slightly different window start/end. **Everything
makes sense and both logs agree.** Mode 3 is solid over a real 30-minute
window, on both sides, not just a lucky short capture.

**One thing this run surfaced that needs fixing before more testing, though:**
peripheral still has `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=y` in its `prj.conf`
(never dropped it after the SENDER-fix diagnosis, unlike central which got this
fix already in `1236752`). At mode-3's traffic volume it's flooding the UART
badly enough to silently corrupt/drop our own app-level prints -- confirmed
144+ messages explicitly marked dropped across 5 gaps, plus at least one
observed mid-line truncation of our own `printk` output. That's almost
certainly why I initially only found one `Periodic sync established.` line
near the end instead of one near the start too -- the real early one likely
got eaten by the flood. Didn't affect the conclusion above (the HCI-level
connect/disconnect counts and the cross-log timing correlation are solid
regardless), but it would make future captures noisier and harder to trust
for precise counts. I'll drop that Kconfig line from `peripheral/prj.conf` to
match central before the next round.

**Remaining open item, unchanged from before:** the actual subevent-count
threshold search (somewhere between 5 and 20) for the `udc` hang is still not
started. Mode 3 confirms 5 works cleanly at full 10s interval; still need to
find where between 5 and 20 it breaks.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — peripheral debug logging dropped, confirmed clean + bonus mode-4 data point

Set `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=n` in `peripheral/prj.conf` (matching
central's earlier fix), rebuilt/reflashed, ran a 30s verification capture --
`logs/peripheral_20260801_111635.log`, pushed. **Confirmed clean: zero `<dbg>`
lines, 27 total lines for 30s** (vs. thousands before). Future captures should
be trustworthy for precise event counts now.

Saw you'd already moved on to `APP_SCALE_TEST=4` (10 subevents) for the binary
search -- my 30s capture happened to land right on that, and it's a good early
sign:

```
Synced to 33:44:4E:21:A0:84 (random) with 10 subevents
Changed sync to subevent 0
>>> Poll received: subevent 0, responding in slot 0
Periodic sync established.
```

Synced cleanly on the first attempt, no errors. That's only 30 seconds though
-- nowhere near enough to call mode 4 confirmed given mode 3 needed a full
30-min run to be sure (and even that had one real resync blip). Worth a longer
soak on mode 4 before concluding 10 subevents is safe and moving further up
the binary search toward 20.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — central's own mode-4 capture agrees, running a 30-min soak next

Central's short capture from the same window (`logs/central_20260801_111659.log`,
pushed) agrees with what you saw on peripheral: clean boot, no `udc` errors,
`Scanning successfully started`, then live readings at the right ~10s cadence:

```
>>> Node 01 (subevent 0): skin_temp=27.00C humidity=48.4% seq=4
>>> Node 01 (subevent 0): skin_temp=27.00C humidity=48.7% seq=6
>>> Node 01 (subevent 0): skin_temp=27.00C humidity=49.3% seq=7
>>> Node 01 (subevent 0): skin_temp=27.00C humidity=50.7% seq=8
```

Agreed on the caution -- a ~30-90s window isn't enough after mode 3 needed the
full 30 minutes to catch its one resync blip. **Running a proper 30-min soak
on mode 4 now** before treating 10 subevents as confirmed and moving further
up toward 20. Will push the result and compare against your side the same way
we did for mode 3.

`common/pawr_protocol.h` currently has `APP_SCALE_TEST=4` (10 subevents, 10s
interval) on central.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — mode 4's 30-min soak: no hang, but 10x higher miss rate than mode 3

Ran the full 30-min soak on mode 4 -- `logs/central_mode4_30min_20260801.log`
(pushed), 11:19:24-11:49:11. **No `udc` errors, no hang, ran the entire
window.** But it's not as clean as mode 3 looked at first glance:

| | seq range | received | miss rate |
|---|---|---|---|
| Mode 3 (5 subevents) | 9-190 (182 expected) | 177 | **2.7%** |
| Mode 4 (10 subevents) | 9-196 (188 expected) | 134 | **28.7%** |

That's a real, roughly 10x jump in missed readings, not noise -- lots of small
1-2 sequence gaps spread throughout the whole run (not clustered at one point
in time), plus one slightly bigger one (37->43, missing 5). No resync/
disconnect events like mode 3's one blip -- this looks like a steady-state
reliability degradation, not an intermittent failure.

**This changes how I'd frame the investigation.** It's not just "find the
subevent count where it hangs" -- there's apparently a reliability *gradient*
starting well before the hard `udc` failure at 20. Worth checking your
peripheral-side capture from the same window for the miss-rate on your end too
(central's `>>> Node 01` count only reflects what made it all the way back to
central -- doesn't distinguish "peripheral never sent it" from "central's
subevent poll missed it" from "response collided/got dropped over the air").
That distinction matters for what the actual fix should be.

**Given this, I'd hold off pushing straight to 15 or a higher count next.**
Might be more useful to understand *why* the miss rate jumped 10x between 5
and 10 subevents first (worth checking `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA=1`
and the other `CONFIG_BT_BUF_*` values noted a few entries back -- small
counts there could plausibly explain exactly this kind of graceful-degradation-
then-hard-failure pattern as subevent count rises) rather than just continuing
the binary search blind to what's actually happening at each step.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — correction: the 28.7% figure conflated two different loss points

Your peripheral soak from the same window landed right after I posted the
above (`logs/peripheral_20260801_111946.log`, pushed) -- worth breaking down
together since it changes the picture:

| Stage | Count (of ~196 expected) | 
|---|---|
| Peripheral generates sensor reading | 189 (near 100%, it's a local timer) |
| Peripheral's `Poll received` (made it over the air) | 164 |
| Central's printed `>>> Node 01` line | 134 |

So there are **two separate loss points, not one**: ~25 readings lost between
"generated" and "peripheral received a poll for it" (over-the-air / subevent
response timing -- this is peripheral's `Failed to receive indication`
count, 16 occurrences, roughly in the right ballpark), and then a **further
~30 lost between "peripheral successfully responded" and "central printed
it"** -- responses peripheral logged as sent that central's log has no record
of at all. That second gap is the more interesting one: it's not explained by
anything visible in peripheral's own log, which means it's specific to
central's side -- either central's subevent-response reception itself, or
something in `response_cb`/the print path silently dropping data.

My earlier "28.7% miss rate" number was real but conflated both of these into
one figure attributed loosely to "reliability degrading with subevent count."
The more precise statement: peripheral's own send-side reliability at mode 4
(164/189 = 86.8%) isn't actually that far off -- the bigger relative loss is
specifically on central's receive side, which fits with `udc`/USB buffer
pressure being the underlying mechanism (central is the one hitting `udc`
buffer exhaustion at higher subevent counts, not peripheral) more precisely
than my first pass did.

Still holding off on pushing further up the binary search until this is
better understood -- if anything this sharpens the earlier suggestion to look
at central's own `CONFIG_BT_BUF_*` sizing specifically, rather than treating
it as a shared/ambiguous cause.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — peripheral's 30-min mode-4 soak (written before seeing the correction above)

[Merge note: this entry and the "correction" entry above it were written
independently around the same time, before either of us had seen the other's
log. Keeping both as originally written since the reasoning trail is useful --
the correction above already reconciles the discrepancy this entry's own math
doesn't catch. Short version: this entry's "180 ~ expected 180" check only
looked at peripheral's own send-side count and didn't yet know about the
~30 responses that never made it into central's printed output at all.]

Ran the full 30-min soak on peripheral in parallel --
`logs/peripheral_20260801_111946.log` (pushed), 11:19:46-11:49:46.

**Result: zero `Connected`/`Disconnected` events for the entire 30 minutes --
sync stayed up continuously the whole time, not even the one resync blip mode
3 had.** 164 successful `Poll received` + 16 `Failed to receive` = 180 total
poll attempts (matches the ~180 expected for 1800s at a 10s interval), plus
189 sensor reads, both right in line with expectations. Zero `<dbg>` lines --
the logging fix is holding, this is a clean, trustworthy capture throughout.

(One capture artifact worth noting so it's not mistaken for a bug: the very
first ~15 lines show several `seq` numbers and `Poll received` lines stamped
within the same millisecond -- that's backlog draining from the board's UART
buffer the moment `Watch-SerialLog` attached, not real simultaneous events.
Settles into genuine ~10s cadence immediately after.)

So mode 4 (10 subevents) looks at least as stable as mode 3, possibly more so
(no blip at all vs. mode 3's one). Waiting on your 30-min central-side result
to cross-check the same way we did for mode 3 -- happy to compare once it's
pushed. If central agrees, seems reasonable to push the binary search further
up (e.g. 15 subevents next) rather than assuming 10 is near the actual
threshold, since we haven't seen mode 4 fail at all yet.

**See the "correction" entry above for the actual cross-check** -- central's
side does show real additional loss (~30 responses) that this entry's own
math didn't have visibility into yet.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — likely root cause found: PAwR response buffer counts, plus first (inconclusive) test

Looked into why this is happening at all, since PAwR is supposed to scale to
hundreds/thousands of nodes per Nordic's own docs -- it's not a protocol
limitation. Found it in `C:\ncs\v3.3.0\nrf\subsys\bluetooth\controller\Kconfig`:
the SoftDevice Controller has two PAwR buffer-count options that were never
set in `central/prj.conf`, sitting at their stock defaults:

- `BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT` (default **3**) -- "Maximum
  number of subevent indications that can be buffered at a time. When using a
  larger value the controller will send data requests for more subevents at
  a time."
- `BT_CTLR_SDC_PERIODIC_ADV_RSP_RX_BUFFER_COUNT` (default **2**) -- "Maximum
  number of periodic advertising response reports that can be buffered at a
  time. This should be increased when a short response slot spacing is used
  so that the controller is able to buffer all responses..."

Neither scales with `NUM_SUBEVENTS` automatically. 2-3 buffers is plausibly
fine at 5 subevents, under real pressure at 10 (matches mode 4's central-side
loss), and probably a contributing factor to the `udc` hang at 20 (controller
backing up trying to service far more subevents than it can buffer, likely
spilling into other USB/HCI resource contention).

**Raised both to 12 in `central/prj.conf`** (commit pending push) and ran a
quick 90s test at mode 4 to sanity-check the build -- `logs/central_20260801_125826.log`,
pushed. Boots clean, no `udc` errors, but the miss rate in this short window
doesn't look obviously better (seq 612,615,616,618,619 -- missing 613/614/617,
5 of 8 in ~90s). **Not drawing a conclusion from this** -- it's a way too short
sample after mode 3 and mode 4 both needed a full 30-min soak to be trustworthy.
Running that soak now before deciding whether this fix actually helped.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — found and fixed a real tooling bug: 30-min soak silently died at 5.5 minutes

The buffer-fix soak I just started stopped writing at 13:06:38 -- only ~5.5
minutes into the requested 30, with no `udc` errors and nothing that looked
like a crash. **`tools/Watch-SerialLog.ps1`'s read loop only caught
`System.TimeoutException` from `$sp.ReadLine()`; any other exception (port
closed unexpectedly, USB hiccup, etc.) fell through uncaught, straight into
the `finally` block, which closed everything cleanly and printed a completely
normal-looking "Log saved to ..." message.** A truncated capture was
indistinguishable from a successful full-length one just by looking at the
tail -- exactly the kind of thing that could have quietly invalidated a
result without anyone noticing.

Fixed: broadened the catch to handle any read error by attempting to
close+reopen the port and continue, and the final message now reports actual
elapsed time vs. requested duration so a short capture can't look identical
to a full one again. Pushed. This wasn't specific to the buffer-count test --
it's been a latent bug in the tool the whole time; earlier 30-min soaks (mode
3, mode 4) happened not to hit it, so their results still stand.

Restarting the buffer-fix soak now with the fixed tool.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — buffer-count fix (TX/RX=12) made things worse, not better: zero output for the full 30 min

Reran the soak with the fixed capture tool. This time it genuinely ran the
full duration -- confirmed via the tool's own new elapsed-time reporting
(**"1800s elapsed of 1800 requested"**, no read errors) -- so the earlier
5.5-minute truncation bug is ruled out as the explanation here. **The file is
completely empty. Zero lines for the entire 30 minutes.** Verified live with
a fresh 20s manual check afterward -- still nothing. Board is not stuck in
bootloader mode, COM port is present and opens fine, RAM usage is only 18%
(47904/262144 B, so not a memory overflow) -- it's producing no serial output
at all, worse than mode 4 pre-fix ever was.

**Raising `BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT` /
`..._RX_BUFFER_COUNT` from 3/2 to 12/12 appears to have broken something, not
fixed it.** Notably: the very first short test right after flashing this
build *did* print a few readings (seq 612-619, in the "inconclusive" entry
above) -- so it's not dead on arrival, it's failing after running for a
while, similar in shape to the original `udc` hang pattern (works briefly,
then goes silent) but without the `udc` error text this time, and without
even getting through a boot banner on the later checks. Possibly 12 is just
too aggressive a jump from the default of 2-3, or there's some other
resource/dependency this trips that isn't documented in the Kconfig help
text.

**Reverting to something more conservative before the next attempt** --
going to try a smaller increase (matching roughly what mode 4's actual demand
would need, closer to NUM_SUBEVENTS=10 than an arbitrary 12/12 -> maybe 6-8)
rather than jumping straight to 12. Will test incrementally this time instead
of guessing a value and running a full 30-min soak on the first try.

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — 6/6 looks promising: clean short capture, zero gaps

Stepped down to `CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT=6` /
`..._RX_BUFFER_COUNT=6` (from the broken 12/12 attempt, and up from the
default 3/2). Short capture right after flashing --
`logs/central_20260801_140928.log`, pushed. **Full clean cycle**: boot,
connect, PAST sent, discovery, GATT write, clean disconnect, scanning
resumed, then **5 consecutive readings with zero gaps** (seq 1,2,3,4,5, no
misses at all in this window):

```
>>> Node 01 (subevent 0): skin_temp=29.50C humidity=46.0% seq=1
>>> Node 01 (subevent 0): skin_temp=29.50C humidity=45.4% seq=2
>>> Node 01 (subevent 0): skin_temp=29.50C humidity=45.4% seq=3
>>> Node 01 (subevent 0): skin_temp=29.50C humidity=45.5% seq=4
>>> Node 01 (subevent 0): skin_temp=29.50C humidity=45.6% seq=5
```

Promising, but same caution as always applies -- a short window isn't proof.
Running the full 30-min soak now (with the now-fixed capture tool, so this
one should be trustworthy start to finish).

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-01 — 6/6 confirmed: real, stable improvement over the full 30 minutes

Full 30-min soak on 6/6 buffer counts -- `logs/central_bufferfix6_30min_20260801.log`
(pushed), 14:11:51-14:41:50, confirmed full duration via the tool's own
elapsed-time check. **Zero `udc`/error lines, ran the entire window, cadence
stayed consistent start to finish.**

Full comparison table now that we have real 30-min numbers for each:

| Config | Subevents | RSP buffers (TX/RX) | Miss rate | Notes |
|---|---|---|---|---|
| Mode 3 | 5 | 3/2 (default) | 2.7% | baseline, low subevent count |
| Mode 4 | 10 | 3/2 (default) | 28.7% | the problem we're fixing |
| Mode 4 | 10 | 12/12 | **total failure** | zero output for 30 min, see two entries back |
| Mode 4 | 10 | **6/6** | **10.6%** | this run |

**6/6 is a real, substantial improvement -- roughly 2.7x better than the
default (28.7% -> 10.6%), no hangs, no `udc` errors, stable for the full
30 minutes.** Not as good as mode 3's 2.7%, but mode 3 only has half the
subevents to service, so that's expected, not a red flag. This also confirms
12/12 wasn't simply "more buffers = better" -- there's a real ceiling
somewhere between 6 and 12 where something breaks outright, worth keeping in
mind before pushing buffer counts up further alongside subevent count later.

**Where this leaves the investigation:** the buffer-count theory is
confirmed as A real contributing factor (not necessarily the *only* one --
10.6% isn't zero), and central's `prj.conf` now has 6/6 instead of the
unset defaults. Given the real target is 20 subevents, not 10, the next
honest step is either (a) test whether 6/6 (or some further-tuned value)
also stabilizes mode 0 at full 20-subevent scale, or (b) first try scaling
the buffer count roughly with subevent count (e.g. ~4-6 per 5 subevents,
so maybe 8-12 for 20 -- but test incrementally given what happened at 12
here) rather than assuming 6/6 generalizes as-is to double the subevent
load.

`central/prj.conf` currently has `CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT=6`
/ `..._RX_BUFFER_COUNT=6`, `common/pawr_protocol.h` still has
`APP_SCALE_TEST=4` (10 subevents).

— Alejandro (session assisted by Claude), 2026-08-01

---

## 2026-08-03 — new lead: reducing printk volume alone avoids the udc hang at full 20-subevent scale

User's hypothesis: the `udc: Failed to allocate net_buf` hang might be
`printk`/USB-console load itself competing with BLE for shared resources,
not (or not only) the PAwR response buffer counts -- worth checking since
this board's console *is* USB CDC-ACM (confirmed via `zephyr,console =
&board_cdc_acm_uart` in the generated devicetree, same USB class as the
failing endpoint 0x80 -- no separate physical UART or RTT probe set up this
session).

**Test setup, isolated from the buffer-count fix on purpose:**
- `central/src/main.c`'s `response_cb` (fires once per received response,
  per subevent, per interval -- the hottest print path, up to 20x every 10s
  at full scale) collapsed from up to 4 separate `printk` calls down to 1.
- `central/prj.conf` buffer counts **reverted to SDK defaults (3/2)** --
  undoing the 6/6 fix from 2026-08-01, specifically so this result isn't
  confounded with that.
- `APP_SCALE_TEST` set to `0` (full production: 20 subevents, 10s interval)
  -- the actual target scale, where the hang has always been worst.

**Result: zero `udc`/error lines over a full 3-minute capture**
(`logs/central_printktest_3min_20260803.log`, pushed) -- the first time all
session a 20-subevent config has survived this long without the hard USB
hang. Previously, unmodified mode-0 hung within seconds of
"Scanning successfully started" every single time.

**Not a clean win yet, though -- flagging honestly:** the onboarding cycle
shows repeated `0x08` (CONN_TIMEOUT) disconnects before finally succeeding
(4 failed attempts, then one that got all the way through GATT write and
started receiving data). This is consistent with the connection-timing
race documented back in Summary.md/PROJECT_STATUS.md fix #6 -- that fix's
margin was tuned assuming the buffer-count fix was also in place; reverting
buffers to defaults for this test likely reopened that timing sensitivity.
So: **the udc hang specifically looks avoided by the printk reduction, but
onboarding reliability at 20 subevents is still rough without the buffer
fix too.**

**Next logical test:** combine both fixes -- reduced printk (now permanent
in `response_cb`) *and* restore the 6/6 buffer counts, both at full
20-subevent scale, and see if that's the combination that's actually stable.
Haven't done that yet; wanted to report the isolated printk-only result
first since it's informative on its own (confirms the user's hypothesis
has real merit, independent of buffer sizing).

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — combined fix (reduced printk + 6/6 buffers) confirms udc hang is gone, but a separate connection-timing issue remains

Restored `CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT=6` /
`..._RX_BUFFER_COUNT=6` (the confirmed 10-subevent fix from 2026-08-01) on
top of the already-permanent reduced-`printk` change, both at full
20-subevent production scale (`APP_SCALE_TEST=0`). 90s test --
`logs/central_20260803_085536.log`, pushed.

**Good news: zero `udc`/`Failed to allocate net_buf` lines, no hard hang,
reached steady state and received live sensor data (seq=18) by the end.**
Double-checked -- the only `udc` matches in the log are the normal `<inf>`
boot lines (`Preinit`, `Initialized`), not the error. Confirms both fixes
together keep the USB hang gone at the real target scale, not just the
printk-only test from earlier today.

**But there's a separate, still-unresolved issue: 3 consecutive `0x08`
(CONN_TIMEOUT) disconnects during onboarding before the 4th attempt finally
succeeded** -- same pattern seen in the isolated printk-only test earlier
today, so **restoring the buffer counts didn't fix this on its own either.**
Given it shows up consistently regardless of buffer count (3/2 or 6/6), this
looks like a genuinely separate problem from the udc hang -- most likely
radio contention specifically during the onboarding GATT connection, since
central is juggling a much busier 20-subevent periodic advertising train at
the same time it's trying to hold a new GATT connection open. This is the
same category of issue the supervision-timeout margin fix (fix #6 in
PROJECT_STATUS.md) was meant to address, but that fix was tuned/verified at
lower subevent counts and may not have enough margin at 20.

**So, current state:** udc hang = fixed (2 independent contributing causes
addressed: printk volume + buffer sizing). Onboarding reliability at 20
subevents = still rough, ~75% of connection attempts timing out before one
succeeds. Not blocking (it does eventually succeed and receive data), but
worth fixing before calling 20 subevents production-ready -- 3 retries taking
~50 seconds before first successful sync isn't acceptable for 17 real nodes
onboarding in sequence.

**Next step:** look at the onboarding connection parameters
(`onboard_conn_param` in `central/src/main.c`) specifically at 20-subevent
scale -- the current values may need the same kind of retuning the buffer
counts needed (i.e., don't assume the existing margin scales to 4x the
subevent count just because it worked at 5).

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — user's stop-PAwR-during-onboarding experiment: confirms the contention theory, but PAST needs advertising running to send at all

User's suggestion: stop periodic advertising entirely during the onboarding
connect step, to test whether radio contention (busy 20-subevent train vs.
new GATT connection) is really what's causing the `0x08` timeouts.
Implemented as a temporary diagnostic flag,
`APP_STOP_PAWR_DURING_ONBOARDING` in `central/src/main.c` -- stops
periodic advertising in `device_found()` right before `bt_conn_le_create()`,
resumes it right after `PAST sent` (not at full onboarding completion,
since the existing post-PAST hold needs a live periodic train for the
peripheral to sync to), with a catch-all resume on the early-failure paths
via the `disconnected:` label. Explicitly a diagnostic, not a proposed fix
-- it desyncs every already-synced peripheral, not just the one onboarding,
since stopping periodic advertising stops it for everyone. Invisible with 1
test peripheral, would be a real cost at 17 nodes.

**Result: `0x08` is completely gone -- every connection attempt succeeds
cleanly now (`Connected (err 0x00)`).** Confirms the contention theory for
that specific symptom.

**But something new appeared: every attempt now fails at the PAST send
itself.**

```
<wrn> bt_hci_core: opcode 0x205b status 0x0c
Failed to send PAST (err -13)
```

`0x205b` = PAST (`LE Periodic Advertising Set Info Transfer`, confirmed
earlier in this project). HCI status `0x0c` = **Command Disallowed** -- the
controller refuses to execute PAST at all in this state. This is almost
certainly self-inflicted by the diagnostic's own ordering, not a new bug:
`bt_le_per_adv_set_info_transfer()` needs the periodic advertising set to
actually be running to have anything to transfer sync info *about* --
periodic advertising is stopped at exactly the moment PAST is attempted in
this flag's current sequencing (stop -> connect -> **PAST while still
stopped** -> resume). Chicken-and-egg: can't send PAST while stopped, but
the flag's whole point was to be stopped during the connect step, which is
also when PAST gets sent.

**So: this experiment has already answered its question (yes, the
contention theory is correct for the `0x08`s), but the flag as currently
sequenced can't be used as an actual fix without restructuring the order --
periodic advertising would need to resume sometime between "connection
established" and "PAST attempted," not stay stopped through both.** Two
ways to explore that if useful: (a) resume right after `Connected` fires
(before PAST), which narrows the contention-free window to just the connect
handshake itself rather than covering PAST too, or (b) accept PAST has to
happen while periodic advertising is running and this approach only ever
helps the connect step specifically, using `onboard_conn_param` retuning
(the option from the previous entry) for whatever contention remains during
PAST itself.

`APP_STOP_PAWR_DURING_ONBOARDING` is currently `1` in `central/src/main.c`
-- central is not currently completing onboarding at all in this state
(every attempt fails at PAST). Don't leave it here.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — user's two follow-up suggestions: first attempt onboards cleanly on the very first try

User's next two ideas, both real Nordic-documented mechanisms for exactly
this kind of concurrency:

1. **`sdc_hci_cmd_vs_allow_parallel_connection_establishments`** -- an SDC
   feature literally described as enabling "establishing connections through
   the initiator and a periodic advertiser with responses simultaneously,"
   disabled by default, reset on every `HCI Reset`. Added a call to
   `hci_vs_sdc_allow_parallel_connection_establishments()` (via
   `<bluetooth/hci_vs_sdc.h>`) right after `bt_enable()` in `main()`, enabling
   it once at boot before any connection/advertising activity starts.
2. **Connection interval aligned to the subevent interval.** Changed
   `onboard_conn_param` from `0x0C` (15ms) to `0x20` (40ms) -- an exact match
   to `PAWR_SUBEVENT_INTERVAL` -- per Nordic's own scheduling-doc guidance
   (colliding roles should share a common factor in their intervals so they
   land on predictable boundaries instead of drifting past each other).

Retired the stop-PAwR-during-onboarding experiment (`APP_STOP_PAWR_DURING_ONBOARDING`
set back to `0`) in favor of these two, since they don't have that
experiment's fatal flaw of needing periodic advertising to be running for
PAST to succeed.

**Result: connected on the very first attempt** -- `logs/central_20260803_103134.log`
(pushed), full clean cycle (`Connected (err 0x00)` -> `PAST sent` ->
`Discovery started` -> `PAwR config written` -> clean `0x16` disconnect, not
`0x08`), then live sensor data. **Zero retries needed**, unlike every
previous 20-subevent test this session, all of which needed 3-4 connection
attempts before one succeeded. Zero `udc` errors too.

One clean cycle could still be luck rather than a real fix -- running a
30-min soak now to confirm this holds, same as every other fix tested today.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — correction: the 30-min soak got interrupted, and the partial data shows the fix isn't as clean as the first test suggested

The soak from the previous entry only ran ~11.5 minutes (10:34:30-10:46:07)
before the session it was running in closed unexpectedly -- not the full 30.
**Important: the partial data on hand is more representative than the single
clean first attempt turned out to be, and it's not good news.**

`logs/central_20subevent_fix_30min_20260803.log` (pushed, partial/incomplete
-- only ~11.5 min, kept for the data it does have):
- **9x `0x08` CONN_TIMEOUT** across 14 connection attempts in that window --
  so the parallel-connection-establishment + interval-alignment fix did NOT
  eliminate the 0x08s at steady state, despite the first attempt connecting
  clean on try 1. That first result was not representative.
- **2x genuine `udc: Failed to allocate net_buf` errors** -- the original
  hang symptom recurred twice in 11.5 minutes, even with reduced printk and
  the 6/6 buffer counts both still active. Board didn't fully lock up either
  time (confirmed still alive and receiving data afterward via a live check),
  but the underlying resource-exhaustion condition this session has been
  chasing all day is clearly still reachable at 20 subevents.

So: **today's full set of fixes (reduced printk, 6/6 buffers, parallel
connection establishment, interval alignment) together reduce the frequency
of both failure modes at 20 subevents, but don't eliminate either one.**
This is consistent with the pattern all session -- every individual fix has
helped, none has been a complete solution at full scale, and problems that
look clean over a 90s window keep turning out to need the full 30-min
treatment to see their real behavior.

(Also: the session interruption left an orphaned PowerShell process holding
COM140 open, requiring a manual `Stop-Process` before the port was usable
again -- not a code bug, just noting it in case it happens again.)

**Restarting a full, uninterrupted 30-min soak now** to get a real number
for the 0x08 and udc rates at 20 subevents with today's fixes, rather than
relying on this partial window.

— Alejandro (session assisted by Claude), 2026-08-03

<!-- New entries go above this line -->
