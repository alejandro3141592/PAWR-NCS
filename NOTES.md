# Collaboration Notes

Shared scratchpad for messages, questions, and suggestions between whoever is
working on `central/` and whoever is working on `peripheral/`. Newest entries
at the top. Sign your entries so it's clear who's asking/answering.

This file is pushed automatically by `tools/Sync-And-Build.ps1` alongside the
serial logs in `logs/`, so it'll show up on the other person's next `git
pull`/`fetch` without either of you needing to remember to push it by hand.

## 2026-08-05 — correction to the 90-min soak entry: "10 node IDs" was 12 physical boards with 2 ID collisions, packet delivery ratio computed

Follow-up after computing the actual packet delivery ratio (PDR) for the
90-min/17-peripheral soak below. Re-examined why central's console only
ever showed 10 distinct node IDs instead of up to 17: **node IDs 32 and
55 each turned out to be two separate physical boards transmitting
concurrently**, not one board or a reassignment artifact -- confirmed by
grouping central's log by `(node_id, subevent)` instead of just
`node_id`: node 32 appeared on both subevent 0 and subevent 10
simultaneously for the entire 90-minute session, each with its own
independently-incrementing `seq` counter (subevent 0: seq 9->550;
subevent 10: seq 84->622, overlapping in time throughout, not
sequential) -- same pattern for node 55 (subevents 2 and 8). This means
**12 physical boards were actually active and reporting**, not 10 --
still short of 17, meaning 5 boards were either not transmitting, not
onboarded, or colliding with an already-stale/reclaimed ID silently, not
yet root-caused. This also fully explains without needing a software
bug: two boards were flashed with the same `CONFIG_APP_NODE_ID` at some
point (today's flashing only touched nodes 63, 61, 34 -- the 32/55
collision predates this session).

**Corrected packet delivery ratio**, computed correctly per physical
board (grouping by `(node_id, subevent)`, using each board's own
sequence-number span as the "expected" denominator, since PAwR/BLE gives
no other ground truth for how many responses *should* have arrived):

```
node subev  received      seq_range  expected     pdr
   8     7       528     20-559           540   97.8%
  21    11       504     84-621           538   93.7%
  31     3       526     38-580           543   96.9%
  32     0       504      9-550           542   93.0%
  32    10       509     84-622           539   94.4%
  34     1       539     37-578           542   99.4%
  35     6       503     48-590           543   92.6%
  55     2       510    272-813           542   94.1%
  55     8       504    142-681           540   93.3%
  61     5       524     35-576           542   96.7%
  63     4       508    113-655           543   93.6%
  64     9       518    195-733           539   96.1%

Physical boards seen: 12
Overall PDR: 95.13% (6177 received / 6493 expected)
```

Range 92.6%-99.4% per board, no single board catastrophically worse than
the others (node 35's 92.6% here is just its normal response-delivery
rate -- separate from its 100%-sensor-failure finding below, which is
about payload *content* being invalid, not about responses failing to
arrive at all).

**Also checked: no node-count limit anywhere in `gui/sensor_gui.py`.**
Table row count (`setRowCount(len(nodes))`) scales to however many
distinct node IDs have been seen, no cap. The only hardcoded ranges found
(`NUM_PERSONS = 4`, a body-silhouette grouping concept unrelated to
per-node data; `range(1, 39)` in the body-part-to-node assignment
dropdown) don't limit how many nodes' data the table/DB/MQTT path can
handle -- 38 as an assignment-dropdown ceiling is well above 17 anyway.
The "only 10 shown" observation was fully explained by the node_id
collision above, not a software limit.

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — first real 17-peripheral 90-minute end-to-end soak: pipeline confirmed working, two real findings

First long-duration test with 17 physical peripherals running
simultaneously. Captured central's console live for the full 90 minutes
(`logs/central_17node_90min_20260805.log`, confirmed "5400s elapsed of
5400 requested" -- not truncated) and pulled gateway_9151's flash log via
SWD (`tools/Read-StorageFlash.ps1`, no UART needed, see the entries below
for why) immediately after the session ended.

**Central's console, full 90 min:** 6183 `>>> Node ...` lines across 10
distinct node IDs (08, 21, 31, 32, 34, 35, 55, 61, 63, 64) -- fewer than
17 because not every physical board had been reflashed with a fresh/known
node ID today, and several likely share overlapping identity with boards
from earlier sessions (see below).

**Gateway's flash log** (8KB FCB, 2 sectors) held 510 clean records (0
CRC mismatches) covering only the tail of the session -- confirmed
in advance this would happen: at 17-node volume the log wraps well
before 90 minutes (sized for ~4 hours at a much lower node count, see
peripheral's original FCB sizing comment). Showed 12 distinct node IDs
(1, 2, 3, 18, 21, 25, 27, 30, 32, 55, 63, 64), several of which (1, 2, 3,
18, 25, 27, 30) **never appear anywhere in central's full 90-minute
log**, while central's node 35 never appears in gateway's dump at all.

**Not a bug -- confirmed with the user:** not all 17 physical boards were
reflashed today, so several are still running node IDs assigned in
earlier sessions. The two logs cover different (only partially
overlapping) time windows anyway (gateway's is tail-only, central's is
the full 90 min), so seeing different node-ID sets in each is expected
once boards can have stale/reused IDs. Sequence numbers in gateway's tail
window start low (0-18) for several nodes, consistent with boards that
power-cycled or freshly joined partway through the session (peripheral's
`s_seq` resets to 0 on reboot).

**Real finding #1: node 35 failed every single reading, the whole
session.** 503/503 occurrences show `skin_temp=0.00C humidity=0.0%`
with both `TEMP_FAIL`/`HUMIDITY_FAIL` flags set, from the very first
reading (`seq=48` at 12:43:29) to the last (`seq=590` at 14:13:22) --
`seq` incrementing normally throughout, so PAwR sync/BLE is fine, this is
specifically that peripheral's I2C sensors (or their wiring) never
working the entire session. Worth a physical check of that board
(loose I2C connection, dead SHT4x/MAX30205, wrong address, etc.).

**Real finding #2: central's own on-board flash log was useless for
this entire session.** `[STORAGE] fcb_append failed (err -28)` (-ENOSPC)
on 6174 of 6183 receptions (99.9%) -- central's 32KB `storage_partition`
was already full from earlier testing today and was never erased/rotated
before this run started (first failure logged within the first second).
**Confirmed this did NOT affect the critical path**: the `>>> Node ...`
UART-forward-to-gateway print happens before `sensor_log_append()` in
`response_cb`, and continued succeeding on every single reception
throughout -- central's flash log is purely a local fallback (per its own
design intent), and its being full only means *that specific fallback*
wasn't available this session, not that any real data was lost from the
gateway/cloud path. FCB is supposed to auto-rotate (overwrite oldest)
when full rather than fail outright -- worth investigating separately
why that didn't happen here (possibly relevant: central has no
`pm_static.yml` pinning its `storage_partition`, unlike gateway_9151 after
the fix in the entries below -- not yet confirmed if that's related).

**Bottom line: the full pipeline (17x peripheral -> BLE -> central -> UART
-> gateway_9151) held up correctly for the entire 90-minute run** --
central kept forwarding every reception to gateway despite its own local
storage being unusable, and gateway's flash log (once readable again
after the security-fault fix) confirms clean, uncorrupted reception on
its end throughout the tail window checked.

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — "gateway isn't uploading to cloud": found and fixed two real bugs (security fault + decoder alignment)

User reported gateway_9151 wasn't reaching HiveMQ Cloud. Console UART0 is
still hardware-dead (see entries below), so this needed the SWD-based
tools built for that problem to diagnose blind.

**Bug 1 (the actual cause of nothing reaching the cloud): `pm_static.yml`
was silently crashing the board on every boot.** `nrfutil device
cpu-register-read --register PC` showed the CPU stuck in
`arch_system_halt` (Zephyr's fatal-error handler), reached via
`z_arm_fatal_error` -> `tfm_ns_fault_handler_callback` -> `arm_fault`,
with `flash_nrf_read` on the call stack -- a genuine TF-M **security
fault** triggered by a non-secure flash read (`sensor_log_init()`'s
`flash_area_get_sectors()`/`fcb_init()`, called near the top of `main()`,
well before UART/MQTT/LTE ever get touched). Root cause: the version of
`pm_static.yml` from the previous entry pinned `settings_storage`/
`tfm_ps`/`tfm_its`/`tfm_otp_nv_counters` as flat leaf partitions but
**omitted their parent SPAN partitions** (`nonsecure_storage`,
`tfm_storage`) that mark the non-secure/secure security boundary in the
original dynamic layout. Without those spans, TF-M had no security
attribution for that flash region at all -- any non-secure access tripped
the SPU. Confirmed reproducible/deterministic: reset the board and
re-read PC -- identical PC/LR/every register, every time, not a
transient/environment-dependent fault. **Fixed** by declaring
`nonsecure_storage` (`span: [settings_storage]`) and `tfm_storage`
(`span: [tfm_ps, tfm_its, tfm_otp_nv_counters]`) explicitly in
`pm_static.yml`, matching the original dynamic layout's boundaries
exactly. Confirmed fixed: rebuilt, reflashed, re-checked PC -- now
sitting in `arch_cpu_idle`'s `wfi` (the same healthy-idle signature
confirmed back in the `console_test`/logic-analyzer investigation), not
`arch_system_halt`.

This is a real lesson about NCS's static partition config, worth
remembering: **the doc's "an `app` entry in a static config is ignored"
note does NOT generalize to every derived/span partition.** `app` is
special-cased; `nonsecure_storage`/`tfm_storage` (and likely other
security-relevant spans on TF-M targets) are not -- omitting them doesn't
error at build time, it silently breaks security attribution and only
surfaces as a runtime fault the first time the affected region is
touched. Worth double-checking with a real boot+register-read (not just
"it built clean") after touching `pm_static.yml` on any TF-M target.

**Bug 2 (found while verifying bug 1's fix): `tools/decode_fcb_dump.py`
had a real alignment bug of its own, unrelated to bug 1.** After the
security-fault fix, `Read-StorageFlash.ps1` found 1 record but flagged it
`crc_ok=False` with garbage values (`node_id=255`, `humidity_pct=5248.0`).
Traced by manually walking the raw bytes at every offset until one
produced both a plausible payload AND a matching CRC. Root cause: FCB's
`fcb_append.c` pads the length-field slot and the CRC slot **separately**
to the flash device's write-block alignment (`fcb_len_in_flash()`,
`f_align` = 4 on both this project's boards, nRF9151 and nRF52840, per
their `write-block-size = <4>` devicetree nodes) -- so a real on-flash
entry for our 8-byte payload is `[1 real len byte + 3 padding][8 payload
bytes][1 real CRC byte + 3 padding]` = 16 bytes, not the naive
1+8+1 = 10 bytes the decoder assumed. Fixed by aligning each slot's size
up to `f_align` (now a `--align` CLI parameter, default 4) when advancing
through entries, while still computing the CRC over only the real
(unpadded) length byte + payload bytes, matching `fcb_elem_crc8()`
exactly. Confirmed fixed against the real device: went from 1
garbage/CRC-failed record to **40 clean records, 0 CRC mismatches**,
all with plausible temp/humidity values from node 55.

**End-to-end confirmed working again**: peripheral -> BLE -> central ->
UART -> gateway_9151 -> flash log, verified via 40 real decoded records.
MQTT/cloud delivery itself was never actually implicated -- the gateway
never got far enough into `main()` to attempt it, so "not uploading to
the cloud" was a symptom of an early boot crash, not an MQTT/LTE/broker
problem. Not yet separately re-confirmed that MQTT publishing itself
works post-fix (the flash log only proves UART reception) -- worth
checking the HiveMQ Cloud dashboard or `gui/sensor_gui.py` directly once
convenient, now that the board is confirmed alive and processing frames.

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — expanded APP_NODE_ID range to 1-100 (was 1-50)

`peripheral/node_id.txt` was set to `55`, which exceeded the Kconfig range
(`1 50`) -- a fresh build would've silently ignored it and fallen back to
the Kconfig default (`1`) with just a warning, not actually flashed as `55`.
Bumped the range to `1 100` for headroom, in three places that all have to
stay in sync (learned this the hard way when it went 17->50 previously):
`peripheral/Kconfig` (`range 1 100`), and `tools/Sync-And-Build.ps1`'s
`-NodeId` param `[ValidateRange(1, 100)]` plus the `node_id.txt` file-parsing
check. Confirmed node 55 builds/flashes/syncs correctly now
(`logs/peripheral_node55_20260805_114336.log`). If you add a fourth node-ID
range check anywhere else, remember these three.

---

## 2026-08-05 — pinned settings_storage's flash address permanently: gateway_9151/pm_static.yml

Follow-up to `tools/Read-StorageFlash.ps1`/`decode_fcb_dump.py` (previous
entry): its `-Address`/`-Size` defaults assumed `settings_storage` stays
at `0xe0000`/`0x2000`, but Partition Manager places that partition
dynamically by default -- nothing actually guarantees it won't move on a
future build that changes flash usage elsewhere. Added
`gateway_9151/pm_static.yml` to pin it permanently, so the script's
defaults keep working without needing `build/pm.config` re-checked after
every build.

**Took three attempts to get the static config right, each teaching
something about how Partition Manager's static-config resolution
actually works (see `nrf/scripts/partition_manager/partition_manager.rst`,
"Adding a static partition"):**

1. Pinning `settings_storage` alone (`address: 0xe0000, size: 0x2000`)
   failed: `Incorrect amount of gaps found in static configuration...
   Gaps found (2): 0x0-0xe0000 0xe2000-0x100000`. Partition Manager's
   static-config algorithm requires **exactly one** contiguous unfilled
   gap in `flash_primary` for dynamic partitions (`app` and friends) to
   fill -- pinning one partition in the *middle* of the region
   necessarily creates a gap on each side of it.
2. Also pinning `tfm` (`0x0`/`0x40000`, the fixed secure image at the
   very start of flash) merged the first gap away, but still failed:
   `Gaps found (2): 0x40000-0xe0000 0xe2000-0x100000`. `settings_storage`
   sits between `app` and the `tfm_ps`/`tfm_its`/`tfm_otp_nv_counters`
   group (all children of `tfm_storage`), so a gap remained on the far
   side too.
3. Followed the doc's own recommended shortcut instead of continuing to
   guess piecewise: copied every `flash_primary` leaf partition from the
   build's own dynamically-generated `gateway_9151/build/partitions.yml`
   (`EMPTY_0`-`EMPTY_3`, `tfm`, `tfm_its`, `tfm_otp_nv_counters`,
   `tfm_ps`, `settings_storage`) into the static file, deliberately
   excluding the derived "span" partitions (`app`, `tfm_secure`,
   `tfm_nonsecure`, `nonsecure_storage`, `tfm_storage` -- Partition
   Manager recomputes these automatically, and the doc says an `app`
   entry in a static config is ignored outright) and the non-flash
   regions (`sram_*`, `nrf_modem_lib_*`, `otp` -- the single-gap rule is
   per-region, and those regions never had the problem). This left
   exactly the one gap that matters (`0x40000`-`0xe0000`, "app") and
   built clean.

Confirmed after the fix: `gateway_9151/build/pm.config` still shows
`PM_SETTINGS_STORAGE_ADDRESS=0xe0000` / `PM_SETTINGS_STORAGE_SIZE=0x2000`
-- unchanged, so `Read-StorageFlash.ps1`'s defaults keep working with no
script changes needed. Build-only verified (not reflashed) as of this
entry -- the pinning takes effect at build time regardless of what's
currently flashed, but the board should be reflashed with this build
before relying on it, same as any other firmware change.

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — UART-free flash log retrieval: tools/Read-StorageFlash.ps1

With console UART0 confirmed hardware-dead on this specific gateway_9151
DK (see the entries below -- logic analyzer directly on the TX pin shows
zero transitions, isolated to the IMCU/VCOM0 path, not firmware),
`CONFIG_APP_DUMP_LOG_ON_BOOT`'s printk-based CSV dump has no way to reach
the PC on this board. J-Link/SWD (used for flashing) is confirmed fully
healthy and is a completely separate physical path from UART0 -- it's how
`west flash` works and how the CPU register reads earlier in this
investigation were done. New tools:

- **`tools/Read-StorageFlash.ps1`** -- reads the `storage_partition` flash
  region's raw bytes over SWD via `nrfutil device read --to-file` (no UART
  involved at all), then calls...
- **`tools/decode_fcb_dump.py`** -- ...to parse Zephyr's FCB (Flash
  Circular Buffer) on-flash format directly from the Intel HEX dump and
  write a CSV, replicating `common/sensor_log.c`'s `sensor_log_dump_all()`
  logic entirely off-device. Format reverse-engineered from
  `zephyr/subsys/fs/fcb/fcb.c`/`fcb_elem_info.c` (entry =
  `[1-byte len][data][1-byte CRC-8/CCITT]`, sector header = 8 bytes
  starting with the `0x50415752`/"PAWR" magic used in `sensor_log_init()`)
  and validated against a synthetic hand-built FCB sector before trusting
  it against real hardware -- decoded both test entries correctly
  (including the flags-based temp/humidity-invalid derivation matching
  the firmware's own logic) with 0 CRC mismatches.

Confirmed working end-to-end against the real board (`nrfutil device
read` -> Python decode -> CSV), currently reporting 0 records since the
partition is empty after the recent reflashes -- the pipeline itself is
verified, just needs the board to actually accumulate UART frames from
`central` before there's real data to retrieve. Usage:
```powershell
./tools/Read-StorageFlash.ps1 -SerialNumber 1051228744
```
`-Address`/`-Size` default to gateway_9151's current
`PM_SETTINGS_STORAGE_ADDRESS`/`_SIZE` (`0xe0000`/`0x2000`, see
`gateway_9151/build/pm.config` after building) -- **these are NOT
guaranteed stable across builds** (Partition Manager places partitions
wherever there's room), so re-check `pm.config` if this ever stops
finding valid records after a build that changes flash layout (e.g. the
NUM_SUBEVENTS/buffer-count changes from 2026-08-04, or any future
Kconfig change affecting partition sizing).

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — correction + strongest evidence yet: P0.26/P0.27 ARE physically probeable, and UART0's TX pin is electrically dead

Correction to a claim in the entry directly below: P0.26/P0.27 (UART0 TX/
RX) are NOT exposed on the Arduino shield connector (confirmed via
Zephyr's own `gpio-map` in `nrf9151dk_nrf9151_common.dtsi`, which only
covers gpio0 pins 0-19/30/31) -- but the nRF9151 DK also breaks out raw
SoC pins on a **separate P0.xx/P1.xx GPIO header** along the board edge,
independent of the Arduino connector. User probed P0.27 (TX) directly on
*that* header with a logic analyzer.

**Result: completely flat, zero transitions.** Not a decode/framing
problem, not a baud-rate mismatch -- the pin is not toggling at the
electrical level at all, full stop.

This is a stronger, more direct result than the uart2-on-P0.23 test
below: that test showed a *different* UART peripheral works, which is
strong circumstantial evidence; this one directly measures UART0's own
TX pin while the firmware is actively trying to drive it (the same
firmware whose `main()` calls `printk()` in a loop every second) and
finds nothing. Combined with the CPU-health confirmation (J-Link register
reads, LED heartbeat) and the uart2 test, this rules out essentially
every remaining possibility on the firmware/software side:
- Not a crashed/hung CPU (confirmed alive and idling normally).
- Not wrong Kconfig/devicetree console routing (confirmed correct:
  `zephyr,console = &uart0`, `status = "okay"`, correct pinctrl/baud).
- Not a UART peripheral hardware defect in general (uart2 on the same
  chip works).
- Not a capture-tooling issue (`Watch-SerialLog.ps1`, raw
  `SerialPort.Read()`, and Nordic's own nRF Connect Serial Terminal all
  agreed -- and now a logic analyzer, with zero software/driver
  dependency, agrees too).

**What's left**, narrowed about as far as this can go without opening
the DK or involving Nordic support: either (a) UARTE0's TX pin isn't
actually being enabled/driven at the SoC level despite the devicetree/
Kconfig looking correct -- possible causes include an IMCU-side pin
default (e.g. GPIO pad configured as input/high-impedance by IMCU
firmware at boot, before the SiP's own pinctrl takes effect) or a pin-
sharing/ownership conflict specific to this pin that isn't visible from
the application's devicetree, or (b) a genuine hardware fault on this
specific physical pin/trace. (a) is more likely given this pin is also
the one the IMCU/VCOM0 path depends on -- i.e. still consistent with an
IMCU-side explanation, just now pinned down to "the SiP pin itself never
gets driven," not merely "the IMCU doesn't forward it to USB."

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-05 — console mystery SOLVED: confirmed IMCU/VCOM0-specific, SiP UART hardware and firmware are fine

Final, conclusive test. Added a second UART to `console_test/` --
`uart2`, a separate SiP UARTE instance disabled by default on this board,
routed to free GPIOs P0.23 (TX) / P0.24 (RX) via a new
`console_test/boards/nrf9151dk_nrf9151_ns.overlay` (see that file for the
full pin-availability check against the board's Arduino header/pinctrl).
Critically, `uart2` is **not** wired to the DK's Interface MCU (IMCU) or
USB at all -- it's a genuinely separate physical path from `uart0`
(console/VCOM0, the one with zero output) and `uart1`
(`arduino_serial`/VCOM1). `main.c` writes to it directly via
`uart_poll_out()`, bypassing `printk()`/the console subsystem entirely,
printing `"[DIAG_UART] tick N"` once a second alongside the existing LED
heartbeat and `uart0`/`printk` output.

Built and flashed clean. User connected a **logic analyzer** directly to
P0.23 (not a UART-to-USB adapter/driver/OS serial stack -- a logic
analyzer reads the raw electrical signal with zero software dependency
of any kind) and **confirmed the DIAG_UART ticks are present and
correct**.

**This closes the investigation.** Combined with everything established
in the two entries below (CPU confirmed healthy via direct J-Link
register reads, sitting in Zephyr's normal idle loop; LED1 blinking
exactly on schedule; zero output on `uart0` across every capture method
tried, including Nordic's own nRF Connect Serial Terminal; a full
physical power cycle changing nothing) plus this result: the SiP itself,
this project's firmware, the build toolchain, and the UART peripheral
hardware in general are all **completely healthy and exonerated**. A
second, independent UART instance on the same chip, in the same firmware
image, works perfectly. The fault is isolated as specifically and
narrowly as this investigation can determine: **the DK's Interface MCU
(IMCU) and/or its VCOM0 routing to the USB-CDC bridge specifically.**
This is downstream of anything `west build`/`west flash`/any code in this
repo touches -- it's either an IMCU firmware issue on this specific
physical board, or a hardware fault in the IMCU-to-SiP UART0 trace/
connection.

**Recommended next step if this needs resolving:** this is now squarely
in Nordic support/DevZone territory, or warrants trying a second physical
nRF9151 DK unit to see if VCOM0 works there (which would confirm this
exact board is faulty, not a class-wide DK issue). `console_test/`
(including the uart2 diagnostic) is left in the repo, ready to flash
again if a second board becomes available for comparison, or if Nordic
support wants a reproducer.

**Practical impact on this project:** `gateway_9151`'s
`CONFIG_APP_DUMP_LOG_ON_BOOT` + reflash retrieval path (unaffected by any
of this, since it never depended on live console output) remains the
working way to read back the on-board flash log. Live console output via
VCOM0 on this specific physical DK cannot be relied on until the IMCU
issue is resolved by Nordic or a board swap.

— Alejandro (session assisted by Claude), 2026-08-05

---

## 2026-08-04 — console mystery: nRF Connect for Desktop's own Serial Terminal also shows nothing

Final piece of evidence, closing off the last remaining "maybe it's this
session's tooling" theory. Rebuilt `console_test/` with an explicitly
broadened config (`CONFIG_SERIAL=y`, `CONFIG_CONSOLE=y`,
`CONFIG_UART_CONSOLE=y`, `CONFIG_LOG_MODE_IMMEDIATE=y`,
`CONFIG_LOG_DEFAULT_LEVEL=3` -- previously only `CONFIG_LOG=y` +
`CONFIG_PRINTK=y` were set, relying on defaults for the rest), pristine
build, reflashed -- still built and flashed clean.

Then, with **nRF Connect for Desktop's own Serial Terminal connected
directly to COM143** (the same official Nordic tool used to check
VCOM0's enabled state earlier, talking through Nordic's own driver
stack, not this project's `Watch-SerialLog.ps1`/`System.IO.Ports`):
**still nothing.** Confirmed directly by the user watching that
terminal live while the freshly-flashed board was running.

This rules out every remaining "maybe it's the capture tooling" angle:
not `Watch-SerialLog.ps1`'s `ReadLine()`, not raw byte-level
`SerialPort.Read()`, and now not even Nordic's own official terminal
app. Every plausible software/tooling explanation has been exhausted.
Combined with the LED-heartbeat proof below (firmware genuinely alive
and executing on schedule) and the J-Link register-read proof (CPU
healthy, normal idle loop), this is as close to fully conclusive as this
investigation can get without either a second physical DK to compare
against or Nordic support involvement: **the fault is in this specific
board's console/VCOM0 hardware or IMCU firmware itself**, not in
anything `west build`/`west flash`/any software here touches.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — console mystery: CONCLUSIVELY isolated to the board's console/VCOM0 path, not app code

Built a brand-new, deliberately minimal app, `console_test/` (sibling to
`central`/`peripheral`/`gateway_9151`, same `nrf9151dk/nrf9151/ns` target)
to settle the question left open in the two entries below. It has
**no modem, no LTE, no MQTT, no UART receiver, no flash log** -- just
`main()` printing via both `printk` and `LOG_INF` once a second, and
toggling LED1 as a heartbeat that doesn't depend on serial at all. Built
clean, flashed successfully (`nrfutil`/`west flash` confirmed), same as
every gateway_9151 build before it.

**Result: LED1 blinks correctly, once per second, exactly as coded --
but the console (COM143/VCOM0, confirmed via `nrfutil device list`)
still shows *zero bytes*, even at the raw OS byte level.**

This is conclusive, not just another data point: combined with the
earlier direct J-Link register reads (CPU confirmed healthy, sitting in
Zephyr's normal idle loop between ticks, see the entry below), a visibly
blinking LED on a 1-second loop proves the firmware is genuinely alive,
correctly executing `main()`'s loop body, on schedule, every time. The
bug is **not in gateway_9151, not in any application code this project
has ever written, and not caused by the on-board flash log, MQTT client,
modem library, or anything else added this session.** It's isolated
entirely to this specific board's console UART0 -> IMCU -> USB-CDC path,
external to anything `west build`/`west flash`/firmware can fix.

Since UART0's pins are internal-only (not exposed on any header, see the
entry below), there's no further diagnosis possible from this side without
either Nordic support, a second physical DK to compare against, or lower-
level IMCU tooling (nRF Connect Programmer's own device log, not yet
tried as of this entry). `console_test/` is left in the repo as a ready-
to-flash diagnostic for next time -- if the same "app is alive, LED
blinks, console silent" pattern shows up again on a different board/setup,
that's the same bug; if a different DK unit shows console output with
this exact app, that would point at this specific unit being faulty.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — gateway_9151 console mystery, continued: physical loopback isn't possible, IMCU/SiP path suspected

Follow-up to the console-output investigation in the entry below (CPU
confirmed healthy via J-Link register reads, zero bytes on either VCOM
port). Two more things ruled out this round:

1. **Full physical power cycle** (unplug USB entirely, wait, replug) --
   no change. This rules out anything about J-Link/debug-probe session
   state or the DK not fully resetting; the earlier tests only ever used
   J-Link-issued soft resets (`RESET_PIN`/`RESET_SYSTEM`), never a true
   power-off.
2. **Raw byte-level capture** (`System.IO.Ports.SerialPort.Read()` into a
   byte buffer, not `ReadLine()`) on COM143 (confirmed VCOM0 via `nrfutil
   device list`), both standalone and with a reset issued mid-capture --
   still **zero bytes at the raw OS level**, not just zero *lines*. Rules
   out a newline/framing mismatch as the explanation.

**Checked whether a physical loopback test (TX->RX jumper + multimeter),
the technique that actually found the earlier VCOM1/`uart_fifo_fill` bug,
is possible here -- it isn't.** Console UART0's pins (TX=P0.27, RX=P0.26,
see `zephyr/boards/nordic/nrf9151dk/nrf9151dk_nrf9151_common-pinctrl.dtsi`)
are internal SiP-to-IMCU pins, not exposed on the Arduino header or any
other accessible pin (unlike UART1/`arduino_serial`, which the earlier bug
used and which *is* exposed on the header). This is a real difference from
the earlier bug, not just "same problem, try the same fix": the earlier
case was a signal-integrity/framing problem on an externally-wired link
between two boards; this one narrows down to something in the path
entirely internal to the DK itself (SiP UARTE0 -> IMCU -> USB-CDC), which
can't be probed with a multimeter or fixed by anything in this repo's
firmware.

**Still unresolved.** Current best guess, unconfirmed: something in the
DK's IMCU firmware/Board Configurator state governing VCOM0's *routing*
(distinct from the simple enabled/disabled flag already checked) is off --
matching the project's own lesson from the VCOM1 case that "shows as
enabled" and "actually routes correctly" are not the same claim. Next
things worth trying if picked back up: nRF Connect for Desktop's
Programmer app has its own device log panel that talks to the IMCU more
directly than a generic COM-port terminal; comparing against a second
physical nRF9151 DK if one becomes available (would distinguish "this
specific unit" from "this whole board family/setup"); or, if available,
Nordic's own DevZone support channel, since Board Configurator internals
aren't something this project's tooling has visibility into.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — clarification: 50 nodes is a fallback ceiling, not the normal operating scale

The 50-node scale-up below (`NUM_SUBEVENTS` 20 -> 55, buffer counts 6/6 ->
15/15) is **headroom for a fallback/edge case, not the expected normal
deployment size**. Normal operation is expected to stay under 20 nodes
(the scale that was actually soak-tested and confirmed stable, see the
2026-08-01 entries below) -- 50 is there so the system doesn't hard-fail if
it's ever pushed past that, not a scale it's meant to run at routinely.
Worth keeping in mind when deciding whether the unverified 15/15 buffer
guess needs a full incremental soak-test pass: if actual deployments stay
under 20 nodes, the 6/6 values that were already validated at that scale
may end up being what's actually exercised in practice, with 15/15 only
matters if/when node count genuinely approaches 50.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — scaled to 50 nodes; dropped gateway_9151's button-dump feature after a real console mystery

**Scaled from 17/20 to 50/55 nodes/subevents.** `NUM_SUBEVENTS` in
`common/pawr_protocol.h` raised 20 -> 55 (50 nodes + 5 spare, matching the
existing "+3 spare" convention scaled up). Subevent train span scales
linearly (55 * 40ms = 2200ms), still well inside the 10s interval (4.5x
headroom vs. 12.5x at 20) -- no interval/subevent_interval/response-slot
timing change needed, this is a straightforward capacity increase, not a
new timing regime. `peripheral/Kconfig`'s `APP_NODE_ID` range widened 1-17
-> 1-50, plus matching doc/comment updates in `common/uart_frame.h`,
`peripheral/src/main.c`, `tools/Sync-And-Build.ps1` (including its
`-NodeId` `ValidateRange`), `BUILD_AND_FLASH.md`, and `gateway_9151/README.md`.

**Buffer counts scaled proportionally, explicitly flagged as unverified.**
`central/prj.conf`'s `CONFIG_BT_CTLR_SDC_PERIODIC_ADV_RSP_TX/RX_BUFFER_COUNT`
raised 6/6 -> 15/15 (20->55 is 2.75x, 6*2.75~=16.5, rounded to 15) as a
starting guess only. Worth remembering why this is flagged so cautiously:
the 2026-08-01 entries below found that these values do NOT scale
automatically with `NUM_SUBEVENTS`, that 6/6 was the result of real
incremental soak-testing (not a first guess), and that jumping too far too
fast (12/12, only 2x up from 6/6, no subevent-count change at all)
*broke* things worse than the stock default (zero serial output for a full
30-min soak) rather than improving them. 15/15 has NOT been soak-tested at
55-subevent scale -- treat it as a starting point for the same
incremental-step methodology (small step, short capture, then a full
30-min soak) before trusting it unattended.

**gateway_9151's button-triggered flash-log dump (added earlier today) was
removed after failing to get working and eating a lot of diagnostic time.**
The feature itself (`dump_button.c`/`.h`, `INPUT_CALLBACK_DEFINE` on
BUTTON1/sw0, `CONFIG_GPIO`+`CONFIG_INPUT`) built and flashed successfully
(confirmed via `nrfutil device fw-verify` matching the on-device image), but
produced **zero serial output on either console port after flashing** --
not even the very first `printk` line in `main()`, across many capture
attempts, multiple resets (including a full `RESET_SYSTEM`), and a
double-checked port identity (`nrfutil device list` showed COM143 = vcom:0/
console, COM142 = vcom:1 -- the *opposite* of `BUILD_AND_FLASH.md`'s
"lower-numbered port is typically VCOM0" heuristic, which is now suspect
and shouldn't be trusted without confirming via `nrfutil device list` on
each machine/setup). Went as far as reading the CPU's program counter
directly over the J-Link (`nrfutil device cpu-register-read`) -- confirmed
the core is genuinely alive and healthy, sitting normally in Zephyr's idle
thread (`arch_cpu_idle`'s `wfi`), not crashed or hung. So the board is
running; something about the console UART path itself (not app logic) is
the mystery, and it wasn't resolved even after trying a separate serial
terminal app outside this session's tooling. **Parked, not solved** -- the
board's actual `CONFIG_APP_DUMP_LOG_ON_BOOT` + reflash retrieval path
(already existing, unaffected by any of this) is what's actually used for
now. If this gets revisited: start from "why is console output missing
even though the CPU is confirmed healthy" rather than re-suspecting the
button/input code, since that's already been ruled out as the cause (the
very first `printk`, before any button-related code runs at all, never
appeared either).

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — central + gateway_9151 now have the same on-board flash fallback log peripheral has

Extended peripheral's on-board flash log (Flash Circular Buffer in the
"storage_partition" Partition Manager reserves, see the 2026-08-03 entry
below) to `central` and `gateway_9151`, so all three hops have a local
fallback record, not just the sensor nodes. Pulled the FCB init/append/dump
logic out of `peripheral/src/main.c` into a shared `common/sensor_log.c|h`
(byte-for-byte port, no behavior change) so central and gateway_9151 reuse
it instead of each reimplementing the same boilerplate.

- **central**: `sensor_log_append()` called in `response_cb` right where it
  already calls `gateway_uart_tx_send()` -- logs every payload received over
  PAwR, as a fallback for both a down UART link to the gateway and a down
  MQTT/LTE hop on the gateway's end. New `CONFIG_APP_DUMP_LOG_ON_BOOT`
  (central's first app-level `Kconfig`, previously only had
  `Kconfig.sysbuild`) for retrieval, same convention as peripheral's
  `CONFIG_APP_DUMP_ON_BOOT`.
- **gateway_9151**: `sensor_log_append()` called in `on_uart_frame` (logged
  on receipt from central, before the MQTT publish attempt -- so the record
  is complete regardless of what happens downstream). Same
  `CONFIG_APP_DUMP_LOG_ON_BOOT` convention added to its existing `Kconfig`.

**gateway_9151 needed real debugging to get working, central didn't.**
Central and peripheral share the same nRF52840 board family and got
`CONFIG_FLASH=y`/`CONFIG_FLASH_MAP=y`/`CONFIG_FCB=y` working immediately
(confirmed: both are plain non-TF-M targets that already have a
`storage_partition` PM region for unrelated reasons -- turned out to be
because their BLE stack enables one of Zephyr's Settings backends as a side
effect, which is what actually triggers Partition Manager's
`settings_storage` reservation, see below). gateway_9151 (nRF9151, TF-M
`/ns`) has no BLE stack and doesn't get that for free, and hit two build
failures before working:

1. `CONFIG_FCB`+`CONFIG_FLASH`+`CONFIG_FLASH_MAP` alone (matching the flag
   set the in-tree `nrf/samples/cellular/modem_trace_flash` sample uses on
   this exact board/target) built fine but **failed to link**:
   `'PM_storage_partition_ID' undeclared`. Root cause, found by tracing
   `FIXED_PARTITION_ID(storage_partition)` through
   `nrf/include/flash_map_pm.h` and `nrf/subsys/partition_manager/Kconfig`:
   Partition Manager only reserves a `settings_storage`/`storage_partition`
   region `if SETTINGS_FCB || SETTINGS_NVS || SETTINGS_ZMS || ...` (see
   `pm.yml.settings`) -- gated on Zephyr's Settings subsystem backend
   choice, not on `CONFIG_FCB`/`FLASH_MAP` directly. There's a separate,
   *fixed* devicetree `storage_partition` node on this SoC
   (`nrf91xx_partition.dtsi`, at `0xf8000`) but it's TF-M's own Protected
   Storage partition, never registered with Partition Manager under that
   name, so it doesn't give the non-secure app a `PM_..._ID` either way.
2. Added `CONFIG_SETTINGS_FCB=y` alone -- **same exact link error again**.
   Traced via `.config-trace.json`: `CONFIG_SETTINGS` was still "not set".
   `SETTINGS_FCB` lives inside Zephyr's `menuconfig SETTINGS` block
   (`zephyr/subsys/settings/Kconfig`), so it's implicitly gated on
   `SETTINGS=y` too, not just its explicit `depends on FCB` -- easy to miss
   since nothing in the symbol's own text says so.
3. Added `CONFIG_SETTINGS=y` as well -- builds and links clean.
   `gateway_9151/build/pm.config` now shows `PM_SETTINGS_STORAGE_ID` (8KB
   at `0xe0000`, inside `nonsecure_storage`), same mechanism as
   central/peripheral, just reached via an explicit two-line Kconfig
   addition instead of a free side effect. Neither `SETTINGS` nor
   `SETTINGS_FCB` is ever actually exercised (no `settings_subsys_init()`/
   handler registered anywhere in this app) -- both enabled purely to
   trigger the PM partition reservation.

Not yet done: none of the three boards has been reflashed/soak-tested with
this change on real hardware yet (verified build-only, all three targets
compile clean as of this entry). Retrieval (`CONFIG_APP_DUMP_LOG_ON_BOOT`)
also untested on real hardware for central/gateway_9151, though it's the
same code path already proven working on peripheral.

## 2026-08-04 — new component: gui/ desktop dashboard (PyQt5 + SQLite)

Added `gui/`, a PyQt5 desktop app that subscribes to the MQTT broker
`gateway_9151` publishes to, shows a live per-node table (temp, humidity,
seq, flags, last-seen), logs everything to a local SQLite DB, and plots a
history chart per node -- adapted from a much richer prior version of this
GUI (`TempUART_reader/scripts/sensor_gui.py`, kept locally, not part of this
repo) that was built for a different sensor set (body-silhouette mapping,
heart rate, SpO2, multiple channels per node). None of that applies to
`gateway_9151`'s actual output (per-node temperature + humidity only, see
`gateway_9151/src/mqtt/mqtt_publisher.c`), so the new version is a plain
table instead of carrying forward UI for data nothing produces.

**Real bugs found and fixed during adaptation, not just copy-pasted:**

1. **Hardcoded plaintext broker password** in the old
   `TempUART_reader/scripts/sensor_gui.py` (`PASSWORD = "..."` at module
   level). New version reads from `gui/config.json` (gitignored -- template
   is the committed `gui/config.example.json`), same pattern as
   `gateway_9151/secrets.conf`.
2. **paho-mqtt v2 API break.** The old code's `mqtt.Client()` (no args) and
   4-arg `on_connect(client, userdata, flags, rc)` are paho-mqtt v1-style --
   confirmed the actually-installed version here is 2.1.0, which deprecates
   the implicit `Client()` constructor and requires
   `callback_api_version=mqtt.CallbackAPIVersion.VERSION2` plus a 5-arg
   `on_connect(client, userdata, flags, reason_code, properties)`. Verified
   by testing against the real installed package rather than assuming the
   reference code's API still applied.
3. **`seq`/`flags` silently never logged** -- caught via a real end-to-end
   test (hardware running, GUI actually receiving and writing to SQLite):
   every row had `seq=None` despite the gateway's JSON payload including
   it. Root cause: `on_message` only ever parsed `nodeId`/`value` out of
   the MQTT JSON, dropping `seq`/`flags` on the floor. Fixed by parsing
   both and threading them through the Qt signal into the DB insert.

**Confirmed working end-to-end on real hardware**: with `peripheral` +
`central` + `gateway_9151` all running, the GUI received live data, logged
correct rows to SQLite (`node_id=2, seq=18/19/20..., flags=0`, real
temp/humidity values matching what the gateway published), no errors.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — CORRECTION + actually resolved: VCOM1 fix wasn't sufficient by itself; central had a real second bug (uart_fifo_fill misuse)

Follow-up/correction to the entry directly below, which declared this fixed
too early. After the VCOM1 fix, real central + real peripheral + real
central-to-gateway wiring, the gateway still received **zero** bytes --
same symptom as before, not a regression, just not actually fixed yet.

**Re-investigated from scratch, this time with central always in the loop**
(the earlier isolated loopback testing, while a legitimate way to test the
gateway's own UART hardware, couldn't have caught a bug that only exists in
central's *sending* code -- an important lesson for next time: a loopback
test that removes one whole side of the link can prove that side's hardware
works, but can't prove the far side's software is correct):

1. Re-verified every gateway-side layer again (interrupt-driven RX ISR: zero
   fires; ISR heartbeat confirmed alive so this wasn't a hung capture) --
   all still correct, nothing new here.
2. Multimeter on the DK's RX pin (P0.28) while central was actively
   transmitting: **2.53V**, not a rail voltage (not 0V, not clean logic-high
   for either board's domain) and not the ~1.8V idle-high measured during
   the earlier same-board loopback test. A real, present, but *abnormal*
   signal -- different enough from "wire is dead" to be a genuinely new
   clue, not the same symptom restated.
3. Chased a voltage-domain-mismatch theory (nRF9151 GPIO configurable
   1.8V/3.3V via Board Configurator, XIAO fixed at 3.3V) far enough to
   nearly recommend sourcing a level shifter -- correctly stopped by
   re-comparing against the working `TempUART_reader` reference project
   first (its actual sender, `arduino_scripts/BLE_reader.ino`, is an
   Adafruit nRF52 -- same 3.3V logic family as the XIAO -- and that setup
   worked with no level-shifting hardware mentioned anywhere, which made
   the voltage-domain theory implausible as the *primary* cause).
4. Full side-by-side diff of `TempUART_reader` vs. `gateway_9151`:
   devicetree overlay resolves identically, and a full generated `.config`
   diff on every `CONFIG_UART_*`/`CONFIG_SERIAL`/`CONFIG_GPIO` symbol came
   back byte-for-byte identical between both projects' actual builds. This
   ruled out every remaining Kconfig/devicetree theory definitively --
   the gateway side's receive path was never the bug.
5. That redirected attention to `central`'s send side, which had never been
   checked this carefully before. Found it in
   `central/src/gateway_uart_tx.c`: `gateway_uart_tx_send()` (called from
   `response_cb`, a BLE callback) used `uart_fifo_fill()` -- whose own
   Zephyr doc comment states *"This function is expected to be called from
   UART interrupt handler (ISR)... Result of calling this function not from
   an ISR is undefined (hardware-dependent)."* `uart_irq_tx_enable()` was
   never called anywhere, so this was never valid usage -- it happened to
   produce *something* on the wire (explaining the 2.53V, not a dead line)
   but not standards-compliant UART framing.

**Fix:** replaced with `uart_poll_out()`, explicitly documented as safe from
any context, blocking one byte at a time until each is queued. At 11
bytes/frame this costs nothing meaningful against the ~500ms-10s gap
between responses.

**Confirmed working immediately after flashing central with this fix**,
gateway still running its raw-byte diagnostic build from the loopback
investigation: clean `0xa5` start bytes, correct 11-byte frames, zero CRC
mismatches, correctly decoded `node=2 seq=10/11/12...` matching central's
own printed values exactly. Real end-to-end pipeline confirmed:
`peripheral` -> BLE -> `central` -> UART -> `gateway_9151` -> MQTT/TLS/LTE
-> HiveMQ Cloud.

**Both fixes were necessary, neither alone was sufficient**:
- VCOM1 disabled (previous entry) -- without this, the gateway's UARTE1
  never gets any signal at all, regardless of what central sends.
- `uart_poll_out()` instead of `uart_fifo_fill()` (this entry) -- without
  this, central's TX never produces standards-compliant framing, so even a
  fully-working gateway receive path sees nothing valid.

Cleaned up the raw-byte/heartbeat diagnostic instrumentation from
`gateway_9151/src/uart/uart_receiver.c` (same kind added during the earlier
loopback investigation) now that the real root cause is confirmed and
fixed. Both `central` and `gateway_9151` rebuilt clean and reflashed with
production code.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-04 — RESOLVED: gateway's UART never receiving was a DK hardware setting (VCOM1), not a code bug

Closing out the investigation from the entries below. Short version: **the
fix was in nRF Connect for Desktop's Board Configurator app, not in any
firmware.** Disabled **VCOM1** for the nRF9151 DK (leaving VCOM0/console
alone), power-cycled, and the gateway's UART link to `central` started
working immediately -- confirmed end-to-end: `central` receiving real BLE
sensor data -> forwarding over UART -> `gateway_9151` decoding frames ->
publishing to HiveMQ Cloud over MQTT/TLS/LTE. No jumper, no code change
required for the actual fix.

**Why this took so long to find:** every layer that could plausibly be
software was checked and ruled out first, in order, with real evidence at
each step:
- Devicetree/pinctrl/IRQ config -- all correct (confirmed against the
  generated `zephyr.dts`/`devicetree_generated.h`, not just source files).
- Init order (UART before modem lib, matching a known-working reference
  project byte-for-byte in structure) -- fixed a real difference, but didn't
  solve this bug.
- Interrupt-driven RX (`uart_irq_rx_enable`) -- ISR confirmed to never fire
  at all, not even once, across thousands of TX attempts.
- Polled-mode RX (`uart_poll_in`, a completely different code path in the
  UARTE driver, bypassing FIFO/interrupts entirely) -- also confirmed zero
  bytes received, while `uart_poll_out` on the same instance was confirmed
  transmitting (incrementing byte values, correct cadence).
- Physical: wire continuity confirmed with a multimeter, correct pins
  triple-checked against the DK's silkscreen, no visible routing switch on
  the board.

That last point turned out to be the key word -- **"visible."** The nRF9151
DK's Arduino header UART is, by default, routed through the DK's Interface
MCU (IMCU) as a virtual COM port (VCOM1), not exposed to the main SiP's
UARTE1 peripheral at all -- controlled by IMCU firmware state, set via a
separate PC app (Board Configurator), not a physical switch or anything
visible by inspecting the board. Nordic's own documentation states this
directly: *"When working with nrf9151dk board with an external MCU host,
you must disable VCOM0 and VCOM1 in the Board Configurator app to release
the UART pins for external use."* There's a real precedent for this exact
class of gotcha on this same DK: a documented Nordic DevZone issue
("no I2C devices ACK on i2c2 (Arduino/Qwiic header)") with the identical
root cause on the I2C bus (`tgt-twi-ctrl` needing to be disabled) -- found
by searching for that I2C issue and recognizing the same IMCU-routing
pattern would apply to UART too.

**Also worth noting for future reference:** the `tempUART_READER` reference
copy (a working prior version of this same UART->MQTT gateway concept, kept
locally for comparison) never mentions VCOM1/Board Configurator anywhere in
its own README/code -- meaning whoever set that up originally hit and
fixed this same DK setting once, outside the repo, and it was never
written down. Worth flagging in case this trips up a fresh DK/setup again
later -- now it's documented here and in `gateway_9151/README.md`.

**Cleanup done:** removed the diagnostic-only code added during this
investigation (`uart_loopback_test.c/.h`, `uart_poll_test.c/.h`, and the
temporary ISR-fire-count/raw-byte instrumentation inside
`uart_receiver.c`) now that the real root cause is confirmed and fixed --
none of it was the actual solution, it was all bring-up diagnostics.
`gateway_9151` is back to a clean production build (exit 0, ~90KB flash,
~47KB RAM) and has been reflashed with the cleaned-up firmware, confirmed
booting correctly (LTE + MQTT connect) via a fresh serial capture.

— Alejandro (session assisted by Claude), 2026-08-04

---

## 2026-08-03 — MQTT/TLS confirmed working on real hardware; UART link to central still not receiving anything

Flashed the TLS build from the previous entry to a real DK: **LTE connects,
TLS handshake to HiveMQ Cloud completes, MQTT connects** -- `[TLS] CA cert
already provisioned`, `[NET] Connected`, `[MQTT] Connected`, `[UART]
Receiver ready`, all as expected. This confirms the whole
LTE/TLS/modem-credential design from the previous entry actually works, not
just compiles.

**But the UART receiver never sees anything from `central`, even though
central is confirmed sending.** Checked in order: central's own console
does show live `>>> Node NN` lines (so it's actively receiving BLE data and
calling `gateway_uart_tx_send()`), the physical wiring is unchanged from
when it was last set up, and the DK has no routing switch gating the
Arduino header UART (user checked the physical board directly). Re-verified
the devicetree side too: pin assignment (P0.29 TX / P0.28 RX), baud (115200
both ends), and confirmed no hardware flow control is enabled on `uart1`
(checked the actual generated `zephyr.dts`, not just the source dtsi) --
software config all looks correct, so this doesn't look like a Kconfig/
devicetree bug the way the last two issues were.

**Added a UART loopback self-test** (`gateway_9151/src/uart/
uart_loopback_test.c`, gated behind `CONFIG_APP_UART_LOOPBACK_TEST`) to
isolate this further: it periodically transmits a fabricated but valid
frame out of the gateway's own UART TX, using the same framing/CRC code
already used for real traffic. Jumper the DK's own TxD2 (P0.29) to its own
RxD2 (P0.28) -- no XIAO involved at all -- and if the gateway's UART
hardware/config is fine, it should receive its own frames back. This
isolates "gateway's UART itself doesn't work" from "the link/wiring to
central specifically doesn't work," which are two different bugs with the
same symptom. See `BUILD_AND_FLASH.md` for the exact build command.

**Not yet run** -- this is the next thing to actually test on hardware,
not a confirmed diagnosis yet.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — first real hardware test: central->gateway UART confirmed, MQTT TLS now implemented

First actual flash+run of `gateway_9151` on a real DK, wired to a running
`central`. Two real bugs found and fixed in sequence:

**1. Central-to-gateway UART link works.** Initially saw nothing arrive at
the gateway; turned out `central`'s own console hadn't been checked, and it
turned out no peripheral was synced yet (nothing to forward). Once a
peripheral was synced and central's console showed live `>>> Node NN`
lines, the gateway's UART receiver worked as expected -- not a bug, just an
empty pipeline upstream.

**2. MQTT couldn't stay connected: `[MQTT] Disconnected: -128`, 5 retries,
`mqtt_publisher_init failed: -116`.** `-128` = `ENOTCONN` (checked against
this toolchain's actual `errno.h`) -- a TCP-level failure before CONNACK,
not a broker-level rejection (would've shown as a small non-zero value via
`MQTT_EVT_CONNACK`, not `MQTT_EVT_DISCONNECT`). Root cause: the gateway was
still using the Kconfig default broker (`test.mosquitto.org`, plain, no
override applied yet) -- discussed reachability options with the user (their
"local Mosquitto" answer doesn't work as-is since the 9151 reaches the
internet over cellular, not the LAN the broker would sit on -- needs
port-forwarding/tunnel/auth thought through first, treated as a separate
future task) and decided to implement real TLS support now and point at the
existing HiveMQ Cloud instance from the old UART_reader project instead.

**TLS implementation, all confirmed against real in-tree references, not
guessed:**
- Fetched HiveMQ Cloud's actual live cert chain (`openssl s_client`) --
  confirmed it's Let's Encrypt (`CN=*.s1.eu.hivemq.cloud`, issued by a Let's
  Encrypt intermediate), so the trust anchor needed is ISRG Root X1.
  Downloaded that root directly from `letsencrypt.org` (not reproduced from
  memory) and checked its SHA-256 fingerprint against Let's Encrypt's
  published value before embedding it in `gateway_9151/src/mqtt/ca_cert.h`.
- `gateway_9151/src/mqtt/tls_provision.c`: this project uses
  `CONFIG_NET_SOCKETS_OFFLOAD=y` (modem-offloaded sockets), so the right
  credential API is `modem_key_mgmt_write()` (AT%CMNG, modem's own storage),
  *not* Zephyr's `tls_credential_add()` -- confirmed against
  `nrf/samples/net/mqtt/src/.../credentials_provision.c`. Hooked via
  `NRF_MODEM_LIB_ON_INIT` -- not just for convenience, but because
  `modem_key_mgmt_write()`'s own doc comment says it returns `-EPERM` when
  the LTE link is active, so the cert must be written before `main()` ever
  calls `wait_for_network()`.
- `mqtt_publisher.c`: `client_init()`/`prepare_fds()` now branch on
  `CONFIG_APP_MQTT_USE_TLS` to use `MQTT_TRANSPORT_SECURE` +
  `mqtt_sec_config` instead of plain TCP.
- Needed two more Kconfig `select`s on `APP_MQTT_USE_TLS`, both found via
  real build/link failures rather than anticipated: `MQTT_LIB_TLS` (without
  it, `MQTT_TRANSPORT_SECURE`/`.transport.tls` don't even exist -- compiled
  out) and `MODEM_KEY_MGMT` (`depends on NRF_MODEM_LIB` but nothing
  auto-selects it, so `modem_key_mgmt_write`/`_exists` were undefined
  references without it). Deliberately did NOT select `CONFIG_MBEDTLS` --
  that's for Zephyr's own software TLS, wrong here since sockets are
  offloaded to the modem's own TLS stack.
- Real credentials (hostname/port/username/password) go in
  `gateway_9151/secrets.conf` (gitignored) -- copy from the committed
  `secrets.conf.example` template, build with `-DEXTRA_CONF_FILE=secrets.conf`
  (see `BUILD_AND_FLASH.md`; note PowerShell mangles that flag if passed
  inline after `--`, needs a variable indirection).

**Result: clean build with real HiveMQ Cloud credentials compiled in --
exit 0, FLASH 131020 B (19.04%), RAM 60600 B (39.28%).** Not yet re-flashed
to hardware to confirm the TLS handshake actually completes against the
live broker -- that's the next thing to verify, not assumed working yet.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — gateway_9151 now builds clean end-to-end; UART pins confirmed

Follow-up to the scaffolding entry just below: got the actual wiring pins
(9151 DK RxD2 = P0.28, TxD2 = P0.29 -- the DK's Arduino-header UART, already
aliased `arduino_serial` in its own devicetree) and pushed `gateway_9151`
through a real `west build` for the first time, rather than leaving it as
untested scaffolding. Two real fixes were needed, both now applied:

1. **`uart1`/`arduino_serial` is disabled by default on the `/ns`
   (non-secure) board variant** this app has to build as -- Nordic's own
   comment in `nrf9151dk_nrf9151_ns.dts` says why: "Disable UART1, because it
   is used by default in TF-M." Fixed with `&uart1 { status = "okay"; };` in
   `gateway_9151/boards/nrf9151dk_nrf9151_ns.overlay` -- confirmed this is
   the standard, intended way to reclaim it (not a hack) by finding the
   in-tree `nrf/samples/peripheral/lpuart` sample doing the exact same
   override for the same conflict on a sibling nRF91 board.
2. **`CONFIG_POSIX_API=y` was missing from `gateway_9151/prj.conf`** --
   without it, `<zephyr/net/socket.h>` only exposes `zsock_`-prefixed names,
   not the plain POSIX ones (`struct addrinfo`, `POLLIN`, `struct pollfd`,
   `getaddrinfo`, `poll`) the MQTT publisher code needs. Also needed
   `#include <zephyr/posix/poll.h>` and `<zephyr/posix/netdb.h>` explicitly
   in `mqtt_publisher.c`.

**Result:** clean build, exit 0 -- FLASH 126848 B (18.43%), RAM 56424 B
(36.58%), board `nrf9151dk/nrf9151/ns`. Build-only, not flashed, per the
standing "don't flash without being asked" rule. New
`BUILD_AND_FLASH.md` at the repo root has copy-pasteable build/flash commands
for all three apps (`central`, `peripheral`, `gateway_9151`) -- worth using
that instead of re-deriving the toolchain env setup by hand each time.

Physical wiring, now confirmed both ends: **XIAO D8 (uart1 TX, P1.13) -> 9151
DK RxD2 (P0.28)**, **XIAO D9 (uart1 RX, P1.14) -> 9151 DK TxD2 (P0.29)**, plus
a shared GND. Not yet actually connected on the bench.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — new feature scaffolded: nRF9151 gateway (BLE data -> MQTT over LTE)

Started a new piece of the system: `gateway_9151/`, a new NCS/Zephyr app for
an nRF9151 DK that will sit behind `central` and republish sensor readings to
an MQTT broker over LTE-M/NB-IoT, so data reaches a server off the local
network. This is scaffolding only -- **not yet built, not yet wired to real
hardware**, see `gateway_9151/README.md` for the itemized "not yet
implemented" list (board overlay/pin choice for the 9151 side, TLS cert
provisioning, first real `west build` attempt).

**Link: `central` <-UART1-> `gateway_9151`.** Added a second physical UART
(`uart1`) to `central` for this, kept deliberately separate from `uart0`
(the existing console, USB-CDC-ACM-backed) so this doesn't compete with the
printk volume that was already a hot-path concern in the udc-hang
investigation above. `central`'s uart1 uses XIAO pins D8/D9 (P1.13/P1.14) --
confirmed free by checking the board's pinctrl (`uart0`'s pins are D6/D7,
I2C sensors already own D4/D5). New file:
`central/boards/xiao_ble_nrf52840.overlay`. `central/src/main.c`'s
`response_cb` now forwards every decoded `sensor_payload` out over this UART
(`gateway_uart_tx_send()`, new `central/src/gateway_uart_tx.c/.h`) right
after the existing `memcpy` -- cheap fire-and-forget `uart_fifo_fill()`,
non-blocking, no-ops harmlessly if no gateway board is wired up. **Central
build-verified clean (exit 0, `.uf2` generated, no new warnings) via `west
build`, board `xiao_ble/nrf52840` -- build only, not flashed**, consistent
with the standing "don't upload the peripheral" instruction from earlier
this session (extended here to mean "don't flash anything without being
asked," central included).

**Wire framing** (`common/uart_frame.h/.c`, shared by both `central` and
`gateway_9151` so they can't drift apart on format): 1 start byte (0xA5) +
the existing 8-byte `sensor_payload` + a 2-byte CRC-16/CCITT-FALSE, 11 bytes
total per frame. No length byte -- payload is fixed-size, and the
`BUILD_ASSERT` on `sensor_payload`'s size in `pawr_protocol.h` already
guards against a silent size mismatch. CRC needed here specifically because
(unlike BLE PDUs) a raw UART byte stream has no link-layer integrity check
of its own.

**Broker is deliberately not fixed to one choice.** Kconfig
(`gateway_9151/Kconfig`) exposes hostname/port/TLS-on-off/username/password/
client-ID as build-time options, so the same firmware can point at a local
Mosquitto (port 1883, no TLS) or a cloud broker like HiveMQ Cloud (port
8883, TLS) without a source change -- per this session's answer of "both."
MQTT client itself is Zephyr's generic `zephyr/net/mqtt.h`
(`CONFIG_MQTT_LIB`), not nRF Cloud's MQTT library, specifically so it isn't
locked to one broker. TLS path is stubbed with a build-time `#error` for now
(see `mqtt_publisher.c`) rather than silently compiling something that can't
actually complete a TLS handshake -- needs real cert provisioning work
first, flagged in the README rather than guessed at.

All new Kconfig/API usage in `gateway_9151/` was checked against real
in-tree NCS v3.3.0 samples before writing (`zephyr/samples/net/mqtt_publisher`
for the MQTT client API + TLS overlay shape, `nrf/samples/cellular/
nrf_cloud_mqtt_device_message` for the modem/LTE Kconfig symbols minus the
nRF-Cloud-specific parts, `zephyr/samples/net/common/net_sample_common.c`
for the Connection-Manager "wait for LTE" pattern) -- not guessed from
memory, given how easy it'd otherwise be to invent a plausible-looking but
wrong Kconfig symbol or API call for a part of NCS neither of us has used
yet on this project.

— Alejandro (session assisted by Claude), 2026-08-03

---

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

---

## 2026-08-03 — full 30-min soak, uninterrupted this time: zero 0x08, zero udc errors

Re-ran the soak start to finish, 10:49:02-11:19:02 --
`logs/central_20subevent_fix_30min_v2_20260803.log` (pushed), confirmed full
1800s via the capture tool's own elapsed-time check. **Zero `0x08`
CONN_TIMEOUT, zero `udc: Failed to allocate net_buf` errors, for the entire
30 minutes.** Only 4 connection attempts total in that window (central only
retries onboarding roughly once per periodic interval when nothing new needs
onboarding), all clean -- 3 clean `0x16` disconnects, 106 sensor readings
received throughout with no gaps in the pattern suggesting instability.

One new, different, and clearly benign thing showed up once: at 11:00:56,
`<wrn> bt_conn: conn ... failed to establish. RF noise?` / disconnect reason
`0x3E` (Connection Failed to be Established -- a different HCI code from
0x08), immediately followed by a successful retry on the very next
connection attempt less than a second later. This reads as ordinary
transient RF-level noise, not a resource-exhaustion pattern like the 0x08s
or udc errors -- flagging it for completeness, not as a concern.

**So: the previous entry's correction was itself about an unusually bad
partial window, not the new steady state.** Between the two runs (11.5 min
partial with 9x 0x08 + 2x udc errors, vs. this full 30 min with zero of
either), the honest read is that today's combined fixes (reduced printk,
6/6 buffers, parallel connection establishment, interval alignment) have
greatly reduced the frequency of both failure modes at 20 subevents, to the
point of not occurring at all in this run -- but the interrupted run proves
they haven't been mathematically proven to zero, and more/longer soaks would
be needed before calling this fully solved. Worth running a few more
independent 30-min soaks (or longer) before treating 20-subevent scale as
production-ready, given the variance already seen between runs today.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — first real multi-node test: 5 peripherals, 1-hour capture. Node 04 never appeared; one node reconnects constantly

Full 1-hour capture of central with all 5 of your freshly-flashed peripherals
(nodes 2-6) connected simultaneously -- `logs/central_5node_1hour_20260803.log`
(pushed), 12:08:56-13:08:56, confirmed full 3600s via the capture tool's own
elapsed-time check. First time this session more than 1 peripheral has ever
been tested at once.

**Good news first:** 20-subevent scale genuinely handles multiple concurrent
nodes -- 4 of the 5 nodes (01, 02, 03, 06) onboarded once, each got a
distinct subevent slot with no collisions, and stayed synced and reporting
for the entire hour without needing to reconnect at all. Zero `udc: Failed
to allocate net_buf` errors the whole hour. Reading counts for those 4:
Node 01: 240, Node 02: 343, Node 03: 232, Node 06: 296 -- all in a
reasonable, consistent range for an hour at a 10s interval.

**Two real problems, though:**

1. **Node 04 has zero readings, the entire hour.** Only 3 distinct
   Bluetooth addresses ever appear anywhere in central's log (one of which
   is the original always-on test peripheral, `F9:FC:23:FC:61:11`) -- Node 04
   never shows up as a connection attempt at all. This isn't a central-side
   onboarding/slot issue; central never even sees this peripheral trying to
   advertise. Points at Node 04's own hardware/firmware -- worth checking
   whether that board is actually powered, running the flashed firmware,
   and advertising as `"PAwR sync sample"` (the exact name central scans
   for) before looking anywhere else.

2. **One peripheral (address `F6:8A:50:0C:25:95`, believe this is Node 05
   given the low reading count below and matching subevent 3) reconnects
   *constantly* -- roughly every 1-2 minutes, all hour, 57+ separate
   onboarding attempts.** Each cycle bounces through a mix of failure modes:
   `<wrn> bt_conn: ... failed to establish. RF noise?` / disconnect `0x3E`
   before remote info is even available, `Timed out during GATT discovery`,
   and `0x08` CONN_TIMEOUT -- sometimes succeeding (`PAwR config written:
   subevent 3`, consistently the same slot each time thanks to the
   address-keyed slot reuse) before dropping again shortly after. Node 05's
   reading count (78) is far below the other 4 nodes' 232-343, consistent
   with spending much of the hour re-onboarding instead of receiving polls.
   Central's own connection-parameter/buffer fixes from earlier today don't
   look like the cause here, since the other 4 nodes on the identical
   central build are rock-solid -- this looks specific to that one physical
   peripheral (RF/antenna/power issue on that board, or something
   board-specific in its own BLE stack init) rather than a central-side or
   protocol-level bug.

**Also, for the record:** 19 `0x08` timeouts total across the hour (all
attributable to the one struggling peripheral's repeated reconnect attempts,
not spread across the other 4) -- so the earlier fix isn't "broken" by
multi-node load, the timeouts are concentrated entirely on the one node with
its own separate problem.

**Suggested next steps:** check Node 04's physical setup first (power,
correct firmware flashed, antenna/board fault) since central never even
detects it. For Node 05 specifically, worth trying a straight reflash or
swapping to a different physical board with the same `node_id.txt` (2) to
see if the problem follows the board or the slot/config -- that would
distinguish a hardware fault on that specific unit from something
config-related.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — correction from user: both findings above explained by physical placement, not bugs

Per the user directly: **there was no Node 04 in this test at all** -- only
5 physical boards were actually deployed (matches central's log, which never
saw a 6th address). Not a missing/broken node, just not part of this test.

**And the Node 05 reconnect-storm has a simple physical explanation: node
placement.** User put Node 03 at 1m from the central/gateway and Node 05 at
2m, specifically to start probing range. Node 03 (1m) was one of the 4
rock-solid nodes with zero reconnects the whole hour; Node 05 (2m) is the one
that reconnected 57+ times. **This lines up as straightforward BLE range/
signal attenuation on this hardware/antenna setup, not a firmware or protocol
bug** -- consistent with the `RF noise?` warnings already seen in that node's
disconnects, which I'd flagged as a symptom without knowing the distance
context.

This is useful, concrete data for the real 17-node deployment: whatever
this hardware's reliable range ceiling is, it's somewhere between 1m
(rock-solid) and 2m (frequent reconnects) under these test conditions
(indoor, whatever obstacles/interference were present). Worth deliberately
mapping this out further -- e.g. a range sweep at fixed distances (1m, 1.5m,
2m, 2.5m...) with everything else held constant -- before finalizing where
the 17 real nodes can physically go relative to the hub.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — two peripheral additions: full sensor resolution, and on-board flash logging for long runs

**1. Temperature resolution fix.** The generic Zephyr `lm75` driver we reuse
for the MAX30205 (register-compatible, but a distinct/finer part) was
truncating readings to 0.5C steps -- its `sensor_channel_get()` conversion
right-shifts the raw 16-bit register by 7 bits before converting, which is
correct for genuine LM75 hardware's 9-bit resolution but throws away almost
all of the MAX30205's real 16-bit/~0.0039C native resolution (per its
datasheet). Added `max30205_read_temp_cdeg()` in `peripheral/src/main.c`,
which reads the same physical register directly via a plain `i2c_dt_spec`
(bypassing the lossy driver conversion entirely) and converts straight to
centi-degrees using the sensor's real 1/256C LSB. **No wire format change**
-- `sensor_payload.temp_cdeg` was already centi-degree resolution, more than
fine enough; this was purely a peripheral-side read-path bug. (Checked the
SHT4x humidity path too -- that driver already preserves full native
resolution all the way through, no equivalent bug there.)

**2. On-board flash logging**, for the ~4h unattended runs user is planning.
User's question: is local flash storage viable, and should we log
everything or just what fails to transmit? Answers:
- **Viable, comfortably.** The board already has a dedicated 32KB "Storage"
  devicetree partition, separate from application code (see
  `nrf52840_partition_uf2_sdv7.dtsi`). At the current 10s interval, 4h is
  ~1440 records * 8 bytes = ~11.2KB -- well under 32KB with real margin.
  Flash endurance (10k erase cycles/page per nRF52840 spec) isn't a
  meaningful concern at this write volume even over years of repeated runs.
- **Log everything, not just failures** -- PAwR gives the peripheral no
  delivery acknowledgment for its subevent responses, so there's no way for
  it to know on-device which readings central actually received. Logging
  only "failed" ones isn't implementable without a much bigger protocol
  change (a return ack channel).
- Skipped the timestamp field idea (`timestamp_ms` in the payload) per
  user's decision -- true wall-clock sync between central/peripheral would
  be needed for cross-device latency and isn't worth the complexity; PAwR's
  own `periodic_event_counter` already gives a shared, sync-free way to
  correlate which cycle a reading belongs to if that's ever needed later.

**Implementation:** `peripheral/prj.conf` now has `CONFIG_FLASH=y`,
`CONFIG_FLASH_MAP=y`, `CONFIG_FCB=y`. New `storage_fcb_init()` (called once
at boot) sets up a Flash Circular Buffer over the "Storage" partition;
`storage_fcb_append()` (called every sensor-read cycle, right alongside the
existing over-the-air send) writes each `sensor_payload` record. Flash
write failures are logged but never block the primary PAwR path -- this is
a fallback, not a new dependency.

**Real gotcha hit and fixed while building this, worth remembering:**
`CONFIG_FCB` only `depends on` `CONFIG_FLASH_MAP` in its Kconfig -- it does
NOT `select` it. Setting `CONFIG_FCB=y` alone left `FLASH_MAP` silently
off, and the build failed at link time with undefined references to
`fcb_init`/`fcb_append`/`flash_area_write`/`flash_area_get_sectors` (not a
compile error, so it wasn't obvious from the source alone). Fixed by adding
`CONFIG_FLASH_MAP=y` explicitly. Confirmed building cleanly now
(`peripheral/build_node1`), **not yet flashed/tested on real hardware** --
user asked to hold off touching the physical peripheral board for now.

Reading back the flash log (for actually retrieving the data after a run)
hasn't been built yet -- would need either a GATT/serial dump command, or
pulling the board and reading it via a debug probe/bootloader-side tool.
Worth doing before the first real long run, not just the write side.

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — first real-hardware test of the 3 peripheral additions found 2 bugs, both fixed

Built and flashed node 2 with all three of today's additions (temp-resolution
fix, FCB flash logging, `dump` shell command) together for the first time on
real hardware. Found two separate problems:

**Bug 1: `CONFIG_SHELL=y` hung the board completely -- zero serial output at
all**, not even the boot banner, confirmed across 3 capture attempts including
one right after a manual reset (ruled out a capture-timing fluke). Isolated
via bisection: guarded the whole `dump` command block (`cmd_storage_dump`,
`storage_dump_walk_cb`, `SHELL_CMD_REGISTER`) in `peripheral/src/main.c` with
`#if defined(CONFIG_SHELL)`, set `CONFIG_SHELL=n` in `prj.conf`, rebuilt --
**full output came back immediately**, confirming the shell backend on our
already-fragile USB-CDC console transport (see the earlier `udc` buffer-
exhaustion bug from higher in this file) is the trigger. Haven't dug into
*why* yet (possibly the shell subsystem's own boot-time banner/prompt
exhausting the same net_buf pool before our first `printk` flushes) --
`CONFIG_SHELL` is off for now, so the `dump` command is currently unavailable.
**Open question for whoever picks this up next: is it worth investigating
further to get `dump` working, or is a debug-probe/bootloader-side flash read
an acceptable fallback for retrieving the log after long runs?**

**Bug 2 (found once bug 1 was fixed and output came back): FCB flash log
failed to init** with `err -35` (`-ENOMSG`) -- Zephyr's FCB code returns this
specifically when a sector's on-flash header magic matches neither "erased"
nor our own magic, i.e. the "Storage" partition had leftover data from
something else (this is the first time this exact partition has ever been
written by this project, so this was always going to surface on first real
use, not a regression). Fixed with the standard FCB recovery idiom in
`storage_fcb_init()`: on `-ENOMSG`, erase the whole partition
(`flash_area_open`/`flash_area_erase`/`flash_area_close`) and retry
`fcb_init()` once. **Confirmed working**:
```
[STORAGE] Flash log area has foreign data, erasing and retrying
[STORAGE] Flash log ready (8 sectors)
```
Flash logging is now genuinely functional on real hardware, not just
compiling cleanly.

Both fixes are in `peripheral/src/main.c`/`prj.conf`, committed and pushed.
Node 2 is currently running with: temp-resolution fix (working, confirmed
real fractional readings like 29.42C), FCB flash logging (working), `dump`
shell command (disabled pending the open question above).

— Alejandro (session assisted by Claude), 2026-08-03

---

## 2026-08-03 — resolved the open shell question: replaced with printk-based retrieval, added a production-quiet toggle

Per the user directly: **not worth chasing why `CONFIG_SHELL` hung the
console** -- dropped it entirely rather than debug it further. Two things
added instead:

**1. Retrieval without shell.** Removed the shell-based `dump` command
(`cmd_storage_dump`, `storage_dump_walk_cb`, `SHELL_CMD_REGISTER`,
`CONFIG_SHELL`, the `#include <zephyr/shell/shell.h>`) entirely. Replaced
with a new Kconfig option, `CONFIG_APP_DUMP_ON_BOOT` (default `n`): when set,
`main()` calls `storage_dump_all()` right after `storage_fcb_init()`, which
walks the FCB and prints the whole log as CSV over plain `printk()` -- the
same console path already proven reliable all session -- then continues
normal PAwR operation as usual. To retrieve a specific board's log: build
with `CONFIG_APP_DUMP_ON_BOOT=y`, flash that board, capture its serial output
right after boot (`tools/Watch-SerialLog.ps1`), the CSV is between the
header row and the trailing `# N rows` line. **Tested and confirmed working
on node 2**: dumped 139 real stored rows (`2,7,0x00,131,31.31,33.8` etc.)
cleanly, then resumed normal sync/response operation immediately after.
Reverted `CONFIG_APP_DUMP_ON_BOOT` back to `n` (its default) once confirmed --
it's opt-in per build, not something to leave on.

**2. Production-quiet toggle.** New Kconfig option `CONFIG_APP_SERIAL_LOGGING`
(default `y`). Added an `APP_LOG(fmt, ...)` macro in `main.c` (wraps `printk`
in `if (IS_ENABLED(CONFIG_APP_SERIAL_LOGGING))`, so the disabled branch is
compile-time dead code, zero runtime cost) and converted every existing
diagnostic `printk()` call in the file to `APP_LOG()` -- boot banner, sensor
reads, connect/disconnect, sync state, storage status, all of it. Set
`CONFIG_APP_SERIAL_LOGGING=n` before flashing for real unattended field
deployment to eliminate any chance of the serial console -- a USB-CDC
transport this session already showed fragile under load twice (`udc`
buffer exhaustion, and the `CONFIG_SHELL` hang) -- being a source of
problems at all. Deliberately does **not** affect the new dump-on-boot
output (still plain, unconditional `printk()`) -- a dump build is a
distinct, intentional retrieval session and shouldn't go silent just
because the quiet flag was left on from a production build.

Both changes are peripheral-only (central wasn't touched -- let me know if
you want the same quiet-logging toggle mirrored there for symmetry, central
usually runs on a bench/gateway machine so the same USB-CDC fragility
concern is less pressing, but happy to add it if useful). Confirmed working
on node 2 with default settings (serial logging on, dump-on-boot off) after
reverting the test flag -- normal boot, flash log init, and sync all
unaffected. Committed and pushed.

— Alejandro (session assisted by Claude), 2026-08-03

## 2026-08-05 — multi-central/multi-gateway coexistence: CONFIG_APP_CENTRAL_ID scoping, this rig assigned ID 1

User's setup will eventually run two or more independent central+gateway rigs
close enough together to be in BLE range of each other. Without any scoping,
any central would GATT-onboard any peripheral it can hear, so a peripheral
meant for rig B could get grabbed by rig A's central. Needed a way to fence
each rig's peripherals to only its own central.

**Design chosen (over a BLE-address-allowlist alternative)**: a new
`CONFIG_APP_CENTRAL_ID` Kconfig int (range 0-999, default 0), mirrored on
both `central/Kconfig` and `peripheral/Kconfig`. `common/pawr_protocol.h`
gained `pawr_format_adv_name()`: a peripheral with `CENTRAL_ID=0` advertises
under the bare `PAWR_ADV_NAME` ("PAwR sync sample") exactly as before; a
non-zero ID appends a numeric suffix (`"PAwR sync sample 1"`). Central builds
its own expected name the same way and only onboards peripherals whose
advertised name matches exactly (`peripheral/src/main.c`'s `adv_name`/`ad[]`
is now built at runtime instead of a `CONFIG_BT_DEVICE_NAME` compile-time
constant; `central/src/main.c`'s `device_found()` filter compares against
`target_adv_name` instead of the old literal). Default 0/0 on both sides is
fully backward compatible -- no config changes needed unless you're actually
running multiple rigs.

**This rig assigned `CONFIG_APP_CENTRAL_ID=1`.** Rebuilt and staged firmware
for the central and all 10 distinct node IDs currently in the field (8, 21,
31, 32, 34, 35, 55, 61, 63, 64 -- covers all 12 physical boards, since node
IDs 32 and 55 each cover 2 physical boards sharing that ID, see the
2026-08-05 PDR correction entry above). All 11 builds (central + 10 node
builds) compiled clean on the first corrected attempt. Build dirs:
`central/build`, `peripheral/build_node<N>` for each N above. None of this
firmware has been physically flashed yet -- staged only, per the user's
choice to build everything first and flash later at their own pace rather
than a live one-at-a-time session. Every board (central and all 12
peripherals) needs reflashing to pick up `CENTRAL_ID=1` -- until reflashed
they're still on the old firmware, which behaves as `CENTRAL_ID=0`
(unscoped) and will keep talking to any central, scoped or not, since a
`CENTRAL_ID=0` peripheral only matches a `CENTRAL_ID=0` central's filter --
i.e. old peripherals simply won't be onboarded by the new `CENTRAL_ID=1`
central until they're reflashed too. Flash via the standard UF2
double-tap-reset + drive-copy process; manual commands follow the same
`Copy-Item ...\zephyr.uf2 -Destination "D:\firmware.uf2"` pattern documented
in `BUILD_AND_FLASH.md`, one board at a time, confirming board identity
before each copy since these boards are visually identical.

**Note for future rigs**: pick a distinct `CONFIG_APP_CENTRAL_ID` per
physical rig (e.g. rig 2 = `CENTRAL_ID=2`) and build every one of that rig's
peripherals with the matching ID plus the central with the same ID. A
peripheral flashed with the wrong ID won't error visibly -- it'll just never
get onboarded by any central in range, since no central's scan filter will
match its advertised name. If a peripheral seems to vanish after a
central-ID change, check its `CONFIG_APP_CENTRAL_ID` first.

— Alejandro (session assisted by Claude), 2026-08-05

## 2026-08-05 — batch-built firmware for node IDs 1-65 (CENTRAL_ID=1), new tools/Build-NodeFleet.ps1

User has node IDs 1-65 in play (beyond the 12 boards physically running
today), and asked for either a full build of every ID or a reusable script
to drive that -- chose the script, since re-scoping or re-ranging will come
up again (new rigs, more boards).

Added **`tools/Build-NodeFleet.ps1`**: takes `-NodeIds <int[]>` and
`-CentralId <int>`, batch pristine-builds `peripheral/build_node<N>` for
each ID with `-DCONFIG_APP_NODE_ID=<N> -DCONFIG_APP_CENTRAL_ID=<CentralId>`.
Build-only, doesn't flash (matches the "build now, flash later at your own
pace over UF2" workflow already in use this session). Central is not built
by this script (different Kconfig option set, only one central per
invocation needed) -- build it separately with the usual `west build`
one-liner (see `BUILD_AND_FLASH.md`), same `-DCONFIG_APP_CENTRAL_ID=N` flag.

Ran `./Build-NodeFleet.ps1 -NodeIds (1..65) -CentralId 1` -- **all 65 builds
succeeded**, verified directly (`build_node<N>/peripheral/zephyr/zephyr.uf2`
present and non-empty for every N in 1-65), even though the script's own
run reported "65 build(s) failed" and exited 1. That failure report was a
bug in the script itself, not the builds: `$results = [ordered]@{}` +
`$results[$n] = $ok` with an *integer* key `$n` hits PowerShell's
ordered-dictionary positional-index overload instead of a real key-value
assignment, throwing `ArgumentOutOfRangeException` on every iteration --
caught by the script's own `$ErrorActionPreference = 'Continue'`, so each
build kept running to completion and succeeded, but every `$results[$n]`
write silently failed, leaving every entry at its default `$false`. Fixed
by switching to a plain `@{}` (unordered) hashtable, which does real
key-based lookup for int keys -- confirmed with a standalone repro. Script
now fixed in the repo; a re-run would report the summary correctly, but was
not re-run since the original 65 builds are already known-good and
re-running would just burn another ~50 minutes for the same output.

Total time for 65 pristine builds: roughly 50 minutes.

Same caveat as the earlier CENTRAL_ID entry applies: nothing is flashed yet,
these are staged `.uf2` files only. To flash a given node N:
```powershell
Copy-Item peripheral\build_node<N>\peripheral\zephyr\zephyr.uf2 -Destination "D:\firmware.uf2"
```
after double-tap-reset puts that board into UF2 bootloader mode.

— Alejandro (session assisted by Claude), 2026-08-05

<!-- New entries go above this line -->
