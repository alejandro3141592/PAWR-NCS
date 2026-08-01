# PAwR Skin Sensor Project — Full Status Summary (2026-08-01)

This is a from-scratch summary of the whole codebase, every bug found and
fixed so far, and the one problem that's still open. `NOTES.md` has the full
blow-by-blow diagnostic trail if you want the details behind any of this;
this file is the condensed version. `Summary.md` is an older version of this
same idea from 2026-07-31 — this file supersedes it.

## What this project is

17 wearable peripheral nodes, each reading skin temperature (MAX30205) and
humidity (SHT4x) over I2C, reporting to one central hub every 10 seconds over
Bluetooth Low Energy using **PAwR** (Periodic Advertising with Responses) — a
BLE 5.4 feature designed for exactly this "one hub, many low-power sensor
nodes" topology. Central just prints received readings for now; no
storage/processing layer yet.

- **Repo**: `https://github.com/alejandro3141592/PAWR-NCS.git`, branch `main`
- **Stack**: nRF Connect SDK (NCS) v3.3.0, Zephyr RTOS. **Not PlatformIO**
  despite the parent folder name — built with `west`/CMake/sysbuild.
- **Hardware**: 2x Seeed XIAO nRF52840 (plain, not the Sense variant) right
  now — one running `central/`, one running `peripheral/`. Target deployment
  is 1 central + 17 peripherals.

## Code layout

- **`central/src/main.c`** — the PAwR advertiser/hub. Runs periodic
  advertising, scans for peripherals named `"PAwR sync sample"`, connects,
  sends PAST (Periodic Advertising Sync Transfer) to hand the peripheral a
  reference to the periodic train, does a GATT discovery + write to assign
  the peripheral a specific subevent/response-slot, disconnects, and repeats
  forever. Every subevent poll response from a synced peripheral is parsed
  into `struct sensor_payload` and printed. Slot assignment is **dynamic and
  hub-driven by design** (not baked into peripheral firmware at compile
  time), specifically so a future scheduler can reassign nodes at runtime and
  support a flexible node count without redesigning the onboarding channel —
  this is a deliberate design decision, not something to simplify away.

- **`peripheral/src/main.c`** — the sync/responder. Advertises connectably,
  receives PAST from central, syncs to central's periodic advertising train,
  reads the two sensors every 10s via a `k_work_delayable`, and answers every
  subevent poll for its assigned slot with the latest reading (sent as
  `BT_DATA_MANUFACTURER_DATA`). Has a status LED (`led0`, red): off = not
  synced, steady on = synced, brief blip = response sent.

- **`common/pawr_protocol.h`** — single source of truth, included by both
  apps. Defines the 8-byte `struct sensor_payload` wire format (node_id,
  flags, seq, temp_cdeg, humidity_pct10 — fixed-point, not float, to avoid
  float-in-BLE-payload portability questions) and all PAwR timing constants
  (subevent count, interval, subevent spacing, response slot delay/spacing).
  **Any protocol or timing change must be made here**, not duplicated in
  either app, so the two sides can't silently drift apart. Currently also
  holds two temporary diagnostic toggles, `APP_MINIMAL_REPRO` and
  `APP_SCALE_TEST` — see "Diagnostic toggles currently in the code" below.

- **`peripheral/boards/xiao_ble.overlay`** — devicetree overlay adding the
  SHT4x (humidity) and MAX30205 (temperature, via the register-compatible
  in-tree LM75 driver) sensors on `&i2c1`, plus a `bias-pull-up` override on
  the `i2c1_default` pinctrl group (see Bug #2 below).

- **`peripheral/Kconfig`** — app-local `CONFIG_APP_NODE_ID` (1-17,
  human-readable label only, does **not** determine subevent assignment —
  that's central's job) and `CONFIG_APP_SIMULATE_SENSORS` (fake sensor
  values for protocol-only testing without real hardware attached).

- **`lib/`** — gitignored, not in the repo. Old Arduino/Bluefruit
  broadcast-based reference from before the PAwR migration, a completely
  different BLE stack. Concepts were mined for the payload design early on;
  the code itself isn't reusable and shouldn't be ported from directly.

- **`tools/Sync-And-Build.ps1`** and **`tools/Watch-SerialLog.ps1`** —
  automation added this session. The first does fetch→commit→push→pristine
  build→flash→serial-capture→push-logs in one command; the second is a
  standalone COM-port logger with timestamps. See their own doc comments for
  usage. `NOTES.md` and `logs/*.log` are the shared collaboration channel
  these push to automatically.

## Bugs found and fixed, in the order they were actually found

1. **Devicetree overlay wasn't being picked up.** Filename-based
   auto-discovery (`boards/<board>.overlay`) silently failed to match for
   this board's qualifier resolution. Fixed by explicitly setting
   `DTC_OVERLAY_FILE` in `peripheral/CMakeLists.txt` before
   `find_package(Zephyr)`. *Symptom if this regresses: a build that doesn't
   show `sht4x@44`/`max30205` in the generated `zephyr.dts`.*

2. **Humidity sensor always read 0.0%.** Root cause: the board's
   `i2c1_default` pinctrl doesn't enable the SoC's internal I2C pull-ups on
   the exposed header pins, unlike Arduino's `Wire.begin()` which does by
   default on the same hardware (confirmed by re-testing a working Arduino
   sketch on identical wiring — ruled out a physical/wiring fault). Fixed by
   overriding `i2c1_default` in the overlay with `bias-pull-up`.

3. **`-ENOMEM` on peripheral's advertising restart.** Peripheral was retrying
   connectable advertising while still connected to central, but
   `CONFIG_BT_MAX_CONN=1` leaves no headroom for a second connection object.
   Fixed with a `sem_disconnected` semaphore so the retry loop waits for
   actual disconnection first.

4. **PAST `skip=1` added an unnecessary full interval of latency** before
   peripheral's first sync attempt — made sense at the original NCS demo's
   much shorter interval, not at this project's longer one. Changed to
   `skip=0`.

5. **Central handed out a fresh subevent slot on every reconnect** from the
   same physical peripheral instead of recognizing it as the same device
   (no identity tracking — just "first free slot"). Fixed by keying slot
   assignment to the peripheral's Bluetooth address (`bt_conn_get_dst`) via a
   `slot_owner[]` table, so a peripheral that retries gets its same slot back
   instead of leaking a new one each attempt.

6. **Supervision-timeout / disconnect-hold race.** Central holds the
   onboarding connection open for roughly one periodic interval after
   sending PAST (to give the peripheral a chance to actually receive at
   least one periodic advertising event before teardown), then disconnects.
   That hold time and the connection's supervision timeout were numerically
   equal, so ordinary radio jitter could make the supervision timeout win the
   race, producing a `0x08` (CONN_TIMEOUT) disconnect instead of a clean
   `0x13`/`0x16` about half the time. Fixed by widening the supervision
   timeout to give real margin over the hold time.

7. **The big one — periodic sync never once succeeded, for most of this
   session.** See "How the real bug was found" below; this was the main
   blocker and took the most work to isolate. Root cause turned out to be a
   single missing Kconfig line, described there.

8. **`Watch-SerialLog.ps1` silently truncated long captures.** The serial
   read loop only caught `TimeoutException`; any other read error (a USB
   hiccup, the port closing unexpectedly) fell through uncaught, ending the
   capture early while still printing a normal-looking "Log saved" message —
   a truncated 30-minute soak looked identical to a successful one just from
   the tail. Fixed by catching all read errors (attempting a reopen-and-retry)
   and having the final message report actual elapsed time vs. requested
   duration, so this can't happen invisibly again.

## How the real bug (periodic sync never succeeding) was found and fixed

This took the most work, so it's worth laying out the actual path, not just
the answer:

1. Enabled `CONFIG_BT_HCI_CORE_LOG_LEVEL_DBG=y` on both sides to get
   HCI-level tracing. Found that **central's PAST send command completes
   with `status 0x00`** (success) every time, but **peripheral's controller
   never logs a single PAST-related HCI event at all** — not even a logged
   failure. This ruled out both sides' *host-level* command handling as the
   cause, and pointed at either an over-the-air issue or a controller
   capability gap.

2. Built a temporary `APP_MINIMAL_REPRO` mode that stripped the project down
   to Nordic's original, much lighter stock sample parameters (5 subevents,
   ~319ms interval, no GATT slot-assignment step) to check whether the bug
   was structural or tied to this project's larger scale. Ran 85+ cycles at
   that light config — **still zero syncs**, ruling out both "the GATT
   round-trip interferes with PAST" and "the 10s/20-subevent scale is the
   problem."

3. Learned that the actual, completely unmodified stock NCS sample pair
   (`periodic_adv_rsp` / `periodic_sync_rsp`) had synced successfully on this
   same hardware a few days prior — meaning the board/controller/SDK
   combination was never actually incapable of PAwR+PAST. Re-ran the literal
   stock `periodic_sync_rsp` sample against this project's own (still
   minimal-repro) central, and **it worked immediately** — continuous
   successful responses for a full 60s capture. That pinned the bug down to
   this project's peripheral code/config specifically, not the environment.

4. Diffed `peripheral/prj.conf` against the stock sample's line by line.
   Found it: the stock sample enables **both**
   `CONFIG_BT_PER_ADV_SYNC_TRANSFER_SENDER=y` and
   `CONFIG_BT_PER_ADV_SYNC_TRANSFER_RECEIVER=y` on the peripheral side, even
   though peripheral only logically *receives* a sync transfer. This
   project's `peripheral/prj.conf` only had `RECEIVER`. **Adding `SENDER`
   fixed it completely** — no build error and no runtime error had ever
   pointed at this; omitting it just silently left out whatever internal
   capability the combined flag pair enables in the SoftDevice Controller.
   This is a genuinely easy trap: nothing in the Kconfig `depends on` graph
   for `RECEIVER` requires `SENDER`, so there's no automated warning if you
   only enable what seems logically necessary.

## The main problem right now

**Periodic sync itself works now** (fix above, confirmed with live sensor
data flowing end-to-end). The current open problem is different: **central
can't reliably run at the full target scale of 20 subevents.**

- At **5 subevents** (light/stock-like config): stable over a full 30-minute
  soak, ~2.7% missed readings (normal/expected miss rate).
- At **10 subevents**: at the SoftDevice Controller's default PAwR response
  buffer sizing, this ran without crashing but with a **28.7% miss rate**
  over 30 minutes — a real, roughly 10x jump, not noise. Root cause: two
  Kconfig options governing how many subevents'-worth of data/responses the
  controller can buffer at once
  (`CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT`, default 3, and
  `..._RX_BUFFER_COUNT`, default 2) were never set and don't auto-scale with
  subevent count.
- At **20 subevents** (the actual target): central hangs — boots fine,
  starts scanning, then hits repeating `udc: Failed to allocate net_buf`
  (USB console buffer exhaustion) and goes completely silent. Never
  produces a `PAST sent` or connects to anything at this scale.
- Raised the two buffer-count options to fix the 10-subevent case. First
  attempt (12/12) made things *worse* — total silence for a full 30-minute
  soak, worse than the unfixed default. Stepped down to **6/6**, which is
  confirmed (full 30-minute soak, clean, no hangs) to cut the miss rate to
  **10.6%** — a real ~2.7x improvement over the default, though not as clean
  as the 5-subevent baseline.

**Not yet resolved:** whether 6/6 (or some other tuned value) fixes the full
20-subevent case, or whether 20 subevents needs proportionally more buffers
than a straight scale-up from 6/6 would suggest — the 12/12 regression shows
this isn't simply "more buffers = better," there's a real ceiling somewhere
that isn't documented. That's the next thing to test.

## Diagnostic toggles currently in the code (temporary, remove when done)

- **`APP_MINIMAL_REPRO`** in `common/pawr_protocol.h` — currently `0`
  (production settings). When `1`, shrinks to 5 subevents / ~319ms interval
  and skips the GATT slot-assignment step, to approximate the stock sample.
  Was used to isolate the SENDER bug above; no longer needed for that, but
  left in place since it's still occasionally useful for fast iteration.

- **`APP_SCALE_TEST`** in `common/pawr_protocol.h` — currently `4` (10
  subevents, full 10s interval), used for the ongoing buffer-count
  investigation above. Mode `2` is retired/invalid (documented in its own
  comment — don't reuse). **Both of these should be removed once the
  buffer-sizing question is settled and the project is confirmed stable at
  the real 20-subevent target** — don't ship with either left on a
  non-default value.

- **`central/prj.conf`** currently has
  `CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX_BUFFER_COUNT=6` and
  `..._RX_BUFFER_COUNT=6` — a deliberate fix, not a diagnostic toggle, but
  worth knowing this is tuned for the 10-subevent test and not yet verified
  at 20.

## Design decisions already made (don't relitigate without a real reason)

- Dynamic, hub-driven slot assignment (central assigns via GATT write) over
  fixed compile-time node IDs — needed for future slot-shifting and flexible
  node-count support.
- No app-layer CRC on the sensor payload — BLE already CRCs every PDU at the
  link layer; redundant for a same-room print-only receiver.
- Fixed-point wire format, not float, to avoid float-in-BLE-payload
  portability questions.
- Real Zephyr sensor drivers throughout (in-tree `sensirion,sht4x` and the
  register-compatible in-tree `lm75` driver for the MAX30205) — no Arduino
  libraries needed or used.

## Build/flash notes

- Plain CLI `west build` from a fresh shell can appear to fail with
  Python-DLL/manifest-resolution errors — this turned out to be a **timeout
  artifact** from a sandboxed shell's shorter command timeout, not a real
  bug; `west build`/`west status` genuinely take a while (tens of seconds to
  a few minutes) walking the NCS workspace's many modules. Runs fine given a
  long enough timeout — this is what `tools/Sync-And-Build.ps1` does.
- Always verify a rebuild actually reconfigured before trusting the `.uf2` —
  an "incremental" build has, more than once, silently not picked up a
  devicetree/CMakeLists change. When in doubt, use `--pristine` (which
  `Sync-And-Build.ps1` always does).
- Flash via UF2 bootloader: double-tap reset, board mounts as a `XIAO-BOOT`
  removable drive; copy `zephyr.uf2` from `<app>/build/<app>/zephyr/zephyr.uf2`
  onto it. Auto-flashes and reboots.
