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

## 2026-08-01 — peripheral's 30-min mode-4 soak is in: even cleaner than mode 3

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

— Alejandro (session assisted by Claude), 2026-08-01

<!-- New entries go above this line -->
