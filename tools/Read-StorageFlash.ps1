<#
.SYNOPSIS
    Retrieves and decodes the on-board flash sensor log (common/sensor_log.c's
    FCB) directly over SWD/J-Link, bypassing UART entirely.

.DESCRIPTION
    Workaround for a hardware fault confirmed 2026-08-05 (see NOTES.md):
    gateway_9151's console UART0 TX pin is electrically dead (confirmed with
    a logic analyzer directly on the pin), isolated to the DK's Interface
    MCU/VCOM0 path -- not fixable from firmware. CONFIG_APP_DUMP_LOG_ON_BOOT's
    printk-based CSV dump therefore can't be captured on this board.

    J-Link/SWD (used for flashing) is confirmed fully healthy and is
    independent of UART0 entirely, so this reads the "storage_partition"
    flash region's raw bytes via `nrfutil device read` and decodes Zephyr's
    FCB (Flash Circular Buffer) format in Python -- no on-device UART
    involved at any point.

    FCB entry format (see zephyr/subsys/fs/fcb/fcb.c, fcb_elem_info.c):
      [1-byte len][len bytes of data][1-byte CRC-8/CCITT over len+data]
    Our payloads are always 8 bytes (struct sensor_payload, see
    common/pawr_protocol.h), so len is always a single byte (0x08), never the
    2-byte varint form (only used for len >= 0x80). CRC-8/CCITT: poly 0x07,
    init 0xFF, MSB-first, not reflected -- confirmed against
    zephyr/subsys/crc/crc8_sw.c's crc8_ccitt() default poly (the 4-bit table
    values Zephyr uses) and CRC8_CCITT_INITIAL_VALUE.

.PARAMETER SerialNumber
    J-Link serial number of the target board. Get it from `nrfutil device list`.

.PARAMETER Address
    Absolute flash address of the storage_partition. Get it from
    PM_SETTINGS_STORAGE_ADDRESS in <app>/build/pm.config after building --
    this is NOT the same on every app/board (Partition Manager places it
    wherever there's room), so don't assume gateway_9151's current value
    (0xe0000, 8KB) applies to a different build without checking.

.PARAMETER Size
    Size of the storage_partition in bytes (PM_SETTINGS_STORAGE_SIZE).
    Defaults to gateway_9151's current 0x2000 (8192).

.PARAMETER OutCsv
    Path to write the decoded CSV to. Defaults to
    logs/storage_dump_<timestamp>.csv under the repo root.

.EXAMPLE
    ./Read-StorageFlash.ps1 -SerialNumber 1051228744
    ./Read-StorageFlash.ps1 -SerialNumber 1051228744 -Address 0xe0000 -Size 0x2000
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$SerialNumber,

    [string]$Address = "0xe0000",
    [string]$Size = "0x2000",
    [string]$OutCsv
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

$sizeBytes = [Convert]::ToInt32($Size, 16)

if (-not $OutCsv) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $logsDir = Join-Path $repoRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
    $OutCsv = Join-Path $logsDir "storage_dump_$stamp.csv"
}

$tmpHex = [System.IO.Path]::GetTempFileName() + ".hex"

Write-Output "Reading $sizeBytes bytes from $Address on board $SerialNumber over SWD..."
& nrfutil.exe device read --address $Address --bytes $sizeBytes `
    --serial-number $SerialNumber --to-file $tmpHex
if ($LASTEXITCODE -ne 0) {
    Write-Error "nrfutil device read failed (exit $LASTEXITCODE)"
    exit 1
}

$pythonScript = Join-Path $PSScriptRoot "decode_fcb_dump.py"
& python $pythonScript $tmpHex $Address $OutCsv
$decodeExit = $LASTEXITCODE

Remove-Item $tmpHex -ErrorAction SilentlyContinue

if ($decodeExit -ne 0) {
    Write-Error "Decode failed (exit $decodeExit)"
    exit 1
}

Write-Output "Decoded CSV written to $OutCsv"
