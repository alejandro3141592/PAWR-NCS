# PAwR smoke test (NCS / Zephyr, not PlatformIO)

Two minimal apps to confirm Periodic Advertising with Responses actually
works between two Seeed XIAO nRF52840 (Sense) boards, per the findings in
`../PAwR/does-pawr-is-supported-idempotent-origami.md`: PAwR needs the
Zephyr Bluetooth stack (nRF Connect SDK), not the Arduino/Bluefruit stack
the main `PAwR` PlatformIO project uses. These are separate from that
project and are **not** built with PlatformIO.

- `central/` — the PAwR advertiser (source of periodic advertising +
  subevent data, receives responses). Based on the NCS sample
  `periodic_adv_rsp`.
- `peripheral/` — the PAwR sync/responder (syncs to the advertiser, echoes
  each subevent payload back as its response). Based on the NCS sample
  `periodic_sync_rsp`.

Both are copied near-verbatim from the NCS v3.3.0 samples at
`C:/ncs/v3.3.0/zephyr/samples/bluetooth/periodic_adv_rsp` and
`periodic_sync_rsp` (Nordic's own tested reference for this exact feature),
with a couple of extra `printk`s so a successful packet round-trip is
obvious in the console output.

## What the test proves

1. `central` starts periodic advertising and, for every subevent, writes an
   incrementing counter byte as the payload.
2. `central` also scans for a device named `PAwR sync sample`. When one is
   found, it connects, transfers periodic sync info (PAST), discovers the
   peripheral's GATT characteristic, and writes it a `{subevent,
   response_slot}` assignment, then disconnects.
3. `peripheral` advertises as `PAwR sync sample`, accepts the connection,
   receives the assignment, and uses PAST to sync to `central`'s periodic
   advertising train on that subevent.
4. Every time `peripheral` receives a subevent packet it prints
   `>>> Packet received: subevent N` and echoes the payload back in its
   assigned response slot.
5. `central`'s `response_cb` prints `>>> Response received: subevent N, slot
   M` with the echoed bytes.

Seeing both `>>> Packet received` (on peripheral) and `>>> Response
received` (on central) in the serial logs is the actual proof PAwR — the
bidirectional part specifically — works on this hardware.

## Prerequisites

You already have NCS v3.3.0 installed at `C:/ncs/v3.3.0` with its toolchain
at `C:/ncs/toolchains/936afb6332`. Build using the same environment the nRF
Connect for VS Code extension uses, i.e. either:

- **nRF Connect for VS Code**: use "Add Build Configuration" on each app
  folder, board target `xiao_ble/nrf52840/sense`, then build/flash from the
  extension's UI, or
- **Toolchain terminal**: open a terminal via the extension's "Open
  Toolchain Terminal" (or `nrfutil toolchain-manager launch --shell` if you
  use nrfutil directly), which sets `ZEPHYR_BASE`, `PATH`, etc. for
  `west`/`cmake`/`ninja`/the arm toolchain automatically.

## Building and flashing

Run from inside a toolchain-configured shell (see above). You need two
boards connected (or flash one, then the other).

```sh
# From C:/Users/mtzal/OneDrive/Dokumente/PlatformIO/Projects/PAwR-ncs

west build -p -b xiao_ble/nrf52840/sense -d central/build central
west flash -d central/build

west build -p -b xiao_ble/nrf52840/sense -d peripheral/build peripheral
west flash -d peripheral/build
```

If your board is the non-Sense XIAO BLE, use board target
`xiao_ble/nrf52840` instead.

`west flash` uses the on-board bootloader/DFU or a debug probe depending on
how your boards are set up — same as any other NCS app for this board; this
isn't PAwR-specific.

## Watching it work

Open a serial monitor on both boards (e.g. `west build -d central/build -t
menuconfig` isn't needed — just any terminal at 115200 8N1 on each board's
CDC-ACM port, or `nrfutil device x-serial-terminal` / VS Code's Serial
Monitor view). Power/reset both. Expected order:

1. `peripheral`: `Waiting for periodic sync...`
2. `central`: `Scanning successfully started` → `Found peripheral ..., connecting...` → `Connected` → `PAST sent` → discovery → `PAwR config written to sync 0, disconnecting`
3. `peripheral`: `Connected` → `New timing: subevent 0, response slot 0` → `Disconnected` → `Synced to ... with 5 subevents` → `Periodic sync established.`
4. Then repeatedly, on `peripheral`: `>>> Packet received: subevent 0` and on `central`: `>>> Response received: subevent 0, slot 0` with matching payload bytes.

## Notes / things that can trip this up

- **Two separate boards required.** One is the advertiser/central, the
  other is the peripheral — flash each app to a different board.
- `central` only connects to a peripheral advertising the name `PAwR sync
  sample` (set via `CONFIG_BT_DEVICE_NAME` in `peripheral/prj.conf`, and
  matched against in `central/src/main.c`'s `device_found()`). Don't rename
  one without the other if you edit these.
- This uses **PAST (Periodic Advertising Sync Transfer)** to get the
  peripheral synced, which is how Nordic's own sample does onboarding. It
  requires a normal GATT connection to happen first (steps 2 above) — that
  connection is torn down once sync + config are handed off; PAwR itself is
  connectionless after that.
- `CONFIG_BT_PER_ADV_RSP` / `CONFIG_BT_PER_ADV_SYNC_RSP` are the Kconfig
  symbols that gate PAwR support in the controller/host — if your
  `west build` fails complaining these are undefined, your NCS version is
  older than needed (want ≥ v2.4.0; you have v3.3.0, so this shouldn't
  happen).
