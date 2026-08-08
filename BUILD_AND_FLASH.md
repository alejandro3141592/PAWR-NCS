# Build & flash cheat sheet

Ready-to-paste commands for all three apps in this repo. Run from a
**PowerShell prompt**, working directory = the repo root (this file's
folder). Each block sets up the NCS toolchain environment fresh, so blocks
are independent -- paste any one on its own, in a new terminal or the same
one.

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
and fill in the real broker hostname/port/TLS/username/password, **and a
client ID unique to this physical board** (`CONFIG_APP_MQTT_CLIENT_ID` --
required if you have or will ever have more than one gateway on the same
broker; the firmware panics at boot if it's left unset, see NOTES.md
2026-08-08).

**Build (plain, no broker secrets -- uses Kconfig defaults, non-TLS local
testing only):** `CONFIG_APP_MQTT_CLIENT_ID` has no default (see gateway_9151/
Kconfig), so even this bench-testing build needs one passed inline:

```powershell
& $python -m west build --build-dir gateway_9151/build gateway_9151 --pristine --board nrf9151dk/nrf9151/ns -- -DCONFIG_APP_MQTT_CLIENT_ID=\"pawr-gateway-local-test\"
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
`printk`, VCOM1 = the Arduino header UART this repo repurposes for the
`central` link, normally disabled per the one-time hardware setup above so
it won't show real traffic here anyway). If both ports look identical in
Device Manager, the lower-numbered one is typically VCOM0/console, but
confirm by capturing a few seconds after a fresh flash/reset -- console
output (`[MAIN] PAwR nRF9151 gateway starting`, etc.) will only appear on
one of them.

Use `tools\Watch-SerialLog.ps1` for timestamped capture to a log file:

```powershell
.\tools\Watch-SerialLog.ps1 -Port COM7 -DurationSeconds 60
```

Replace `COM7` with the actual port (Device Manager, or `[System.IO.Ports.SerialPort]::GetPortNames()`).
