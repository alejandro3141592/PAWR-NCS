# Build & flash cheat sheet

Ready-to-paste commands for all three apps in this repo. Run from a
**PowerShell prompt**, working directory = the repo root (this file's
folder). Each block sets up the NCS toolchain environment fresh, so blocks
are independent -- paste any one on its own, in a new terminal or the same
one.

## `builds/` -- staged output artifacts (2026-08-13)

`builds/` at the repo root holds **copies of already-built `.uf2`/`.hex`
files**, organized by purpose -- not `west build --build-dir` targets
themselves (those stay exactly where they've always been: `central/build*`,
`peripheral/build*`, `gateway_9151/build`). This is a separate, parallel
staging area, mainly useful when preparing several boards' firmware ahead of
a flashing session (build everything first, flash at your own pace later),
or when you want a clearly labeled copy of what's currently on a board
without digging through a `build_node<N>` working directory.

- **`builds/latest/`** -- current production firmware: `central1/`,
  `central2/` (one folder per central rig), `gateway/` (note: `merged.hex`
  is the correct file for `west flash`/`nrfjprog`, not `zephyr_ns_only.hex`
  -- see the gateway section below for why), and `peripheral_template/` (a
  reference build with Kconfig defaults, NOT meant to be flashed to a real
  node -- see the README in that folder).
- **`builds/extraction/`** -- one-shot data-retrieval firmware
  (`CONFIG_APP_DUMP_ON_BOOT=y`), one folder per node currently being pulled
  from. See "Retrieving a node's full on-board flash log" below for the full
  workflow this supports.

Both folders are just staged copies -- after flashing from one, the
source-of-truth build directory (`peripheral/build_node<N>_dump`, etc.) is
still what you'd rebuild from if you need to regenerate it. Re-copy the
output here manually after any rebuild if you want `builds/` to stay current
-- nothing does this automatically.

This mirrors exactly how `tools/Sync-And-Build.ps1` sets up its environment
(same toolchain path, same env vars) -- if you'd rather not paste this by
hand every time, that script already automates fetch/build/flash/log-capture
for `central`/`peripheral`. This file exists for quick one-off builds, or for
`gateway_9151`, which the script doesn't support yet (different board
family, no on-board UF2 bootloader).

## One-time setup (paste this first in a new terminal)

```powershell
$tc = 'C:\ncs\toolchains\936afb6332'
$env:PATH = "$tc\opt\bin;$tc\opt\bin\Scripts;$tc\mingw64\bin;$tc\bin;$tc\cmd;$tc\usr\bin;" +
            "$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;" +
            "$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_BASE = 'C:\ncs\v3.3.0\zephyr'
$python = "$tc\opt\bin\python.exe"
```

Everything below assumes this has already been run in the current terminal
session (shell state doesn't persist between separate terminal windows/tabs).

---

## central (Seeed XIAO nRF52840 -- PAwR hub)

**Build:**

```powershell
& $python -m west build --build-dir central/build central --pristine --board xiao_ble/nrf52840
```

Use `xiao_ble/nrf52840/sense` instead if your physical board is the Sense
variant (check which one you actually have -- see `README.md`'s board target
note).

**Flash:** the XIAO has no J-Link -- it flashes via its on-board UF2
bootloader (double-tap reset to enter bootloader mode, board enumerates as a
USB mass-storage drive):

```powershell
Copy-Item central\build\central\zephyr\zephyr.uf2 -Destination "D:\firmware.uf2"
```

Replace `D:` with whatever drive letter the board actually enumerates as in
bootloader mode (check `Get-Volume` or File Explorer) -- it varies by
machine/USB port. Board reboots into the new firmware automatically once the
copy finishes.

---

## peripheral (Seeed XIAO nRF52840 -- skin sensor node)

**Build** (set `-DCONFIG_APP_NODE_ID=N` to this board's node number, 1-50;
omit it to use whatever's already in `peripheral/prj.conf`/`peripheral/node_id.txt`):

```powershell
& $python -m west build --build-dir peripheral/build peripheral --pristine --board xiao_ble/nrf52840 -- -DCONFIG_APP_NODE_ID=1
```

**Flash** (same UF2 drag-and-drop method as central):

```powershell
Copy-Item peripheral\build\peripheral\zephyr\zephyr.uf2 -Destination "D:\firmware.uf2"
```

Again, replace `D:` with this board's actual bootloader-mode drive letter.

**Prefer the automated script for this one.** `peripheral` is the app you'll
rebuild most often (once per physical node, per node ID) -- `tools\Sync-And-Build.ps1`
already handles the node-ID Kconfig flag, per-node build directories, flashing,
and serial log capture in one step:

```powershell
.\tools\Sync-And-Build.ps1 -App peripheral -NodeId 1
```

### Retrieving a node's full on-board flash log (2026-08-13)

Every peripheral keeps its own local flash log of every reading it's ever
taken (`common/sensor_log.c`'s FCB), independent of whether that reading
ever made it over PAwR to central. To pull a specific node's complete
history off the device itself (not just whatever trickled into the GUI's
database live), build and flash that node with
**`CONFIG_APP_DUMP_ON_BOOT=y`** added to the usual node build:

```powershell
& $python -m west build --build-dir peripheral\build_node<N>_dump peripheral --pristine --board xiao_ble/nrf52840 -- -DCONFIG_APP_NODE_ID=<N> -DCONFIG_APP_CENTRAL_ID=<C> -DCONFIG_APP_DUMP_ON_BOOT=y
```

Flash it the normal UF2 way. On every boot (including the very next one,
right after this flash), the node prints a 5-second countdown, then its
entire flash log as CSV (`node_id,seq,flags,row,temp_c,humidity_pct`) over
its console, throttled to ~10ms/row specifically so the console doesn't
silently drop lines under the burst -- then resumes normal PAwR operation.

**Capturing that dump reliably needs more than `tools\Watch-SerialLog.ps1`.**
That tool is `ReadLine()`-based (line-buffered), and a real dump of
1000+ rows was confirmed on real hardware (2026-08-13, node 5, 1152 rows) to
lose roughly half its lines that way even with the firmware's own
throttling -- the console's own buffering plus `ReadLine()`'s stall
semantics compound under the burst. Use a raw byte-stream capture instead
(reads continuously via `pyserial`'s `Serial.read()`, no line
buffering/blocking, reconnect-resilient since a board reset briefly drops
and re-enumerates its USB CDC port):

```powershell
python tools\capture_flash_dump.py COM<port> <duration_seconds> <output_file>
```

(See `tools/capture_flash_dump.py`.) Confirm a complete capture by checking
the dump's own trailer line (`# <N> rows`) against the `flash_row` column in
your parsed output -- it should be a contiguous `0..N-1` sequence with no
gaps. A partial/dropped capture still looks superficially fine (valid CSV
rows, no corruption) but silently under-counts, so this check matters, don't
skip it.

Once you have what you need, rebuild that node WITHOUT
`-DCONFIG_APP_DUMP_ON_BOOT=y` (plain `Sync-And-Build.ps1`/manual build) and
reflash it, so it doesn't keep re-dumping its whole history on every future
reset.

---

## gateway_9151 (nRF9151 DK -- BLE-to-MQTT/LTE gateway)

**One-time hardware setup, before this will ever receive UART data:**
disable **VCOM1** for this DK via nRF Connect for Desktop's **Board
Configurator** app (connect to the DK, disable VCOM1, leave VCOM0 alone,
apply, power-cycle). Without this the DK's Interface MCU holds the Arduino
header's UART pins (TxD2/RxD2) as its own virtual COM port and the SiP's
UARTE1 never sees any RX data at all -- confirmed the hard way, see
`gateway_9151/README.md` and `NOTES.md` 2026-08-03/04. This is a one-time
DK configuration, not something the build/flash commands below can fix.

**One-time, before the first build:** copy `gateway_9151/secrets.conf.example`
to `gateway_9151/secrets.conf` (gitignored -- never commit real credentials)
and fill in the real broker hostname/port/TLS/username/password.

**Build (plain, no broker secrets -- uses Kconfig defaults, non-TLS local
testing only):**

```powershell
& $python -m west build --build-dir gateway_9151/build gateway_9151 --pristine --board nrf9151dk/nrf9151/ns
```

**Build with real broker credentials (HiveMQ Cloud / TLS, or your own
Mosquitto settings) from `secrets.conf`:**

```powershell
$extraConf = "-DEXTRA_CONF_FILE=secrets.conf"
& $python -m west build --build-dir gateway_9151/build gateway_9151 --pristine --board nrf9151dk/nrf9151/ns -- $extraConf
```

Note the `$extraConf` variable indirection -- PowerShell's argument parsing
mangles `-DEXTRA_CONF_FILE=secrets.conf` if passed as a literal inline
argument after `--` (splits it into two words at the `.`), so assign it to a
variable first and pass that instead.

`/ns` = non-secure image (the normal way nRF91 apps are built under TF-M) --
confirmed correct for this board via a real successful build, see
`gateway_9151/README.md`.

**Flash:** the DK has an on-board J-Link debugger, so `west flash` handles
programming directly over USB (no bootloader-drive drag-and-drop, unlike the
XIAO boards) -- needs `nrfjprog`/J-Link tools installed, which the toolchain
environment above already puts on `PATH`:

```powershell
& $python -m west flash --build-dir gateway_9151/build
```

**Confirmed working end-to-end on real hardware** (2026-08-04): LTE
connects, TLS handshake to HiveMQ Cloud completes, MQTT connects, and the
UART link to `central` receives real sensor frames (once VCOM1 is disabled
per the one-time hardware setup above).

---

## Serial monitoring

`central`/`peripheral` (XIAO boards) enumerate as a single USB CDC-ACM
serial port for `printk` output. `gateway_9151` (nRF9151 DK) enumerates as
**two** "JLink CDC UART Port" COM ports -- one per VCOM (VCOM0 = console/
`printk`, **and since 2026-08-12 also the fallback sensor-data feed to
`gui/sensor_gui.py`, see `gateway_9151/src/uart/gui_uart_tx.c`** -- VCOM1 =
the Arduino header UART this repo repurposes for the `central` link,
normally disabled per the one-time hardware setup above so it won't show
real traffic here anyway).

**Do NOT use "the lower-numbered port is typically VCOM0" as a heuristic --
confirmed wrong on real hardware more than once** (see `NOTES.md`, and
`gui/sensor_gui.py`'s `find_gateway_ports()` fix 2026-08-12, where port
auto-detection picked the wrong VCOM using exactly this assumption and
silently ingested nothing for an entire session). The reliable way to tell
them apart is to ask `nrfutil` directly, which reports each port's real
vcom index:

```powershell
nrfutil device list --json --traits jlink,serialPorts
```

Look for `"comName"` paired with `"vcom":0` in the output -- that's the
console/data port. (`gui/sensor_gui.py`'s `_find_gateway_ports_via_nrfutil()`
does exactly this programmatically, if you want a working reference
implementation.) Confirming by capturing a few seconds after a fresh
flash/reset and checking for `[MAIN] PAwR nRF9151 gateway starting` also
works, but is slower and requires a fresh reset each time you're unsure.

Use `tools\Watch-SerialLog.ps1` for timestamped capture to a log file:

```powershell
.\tools\Watch-SerialLog.ps1 -Port COM7 -DurationSeconds 60
```

Replace `COM7` with the actual port (Device Manager, or `[System.IO.Ports.SerialPort]::GetPortNames()`).

**For high-volume/bursty output (e.g. a `CONFIG_APP_DUMP_ON_BOOT` flash-log
dump), use `tools\capture_flash_dump.py` instead** -- `Watch-SerialLog.ps1`
is line-buffered (`ReadLine()`) and confirmed on real hardware to silently
drop roughly half the lines of a 1000+ row burst even with firmware-side
throttling. See "Retrieving a node's full on-board flash log" above for the
full explanation and usage.
