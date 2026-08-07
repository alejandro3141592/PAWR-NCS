<#
.SYNOPSIS
    Batch-build peripheral firmware for a range/list of node IDs, each
    scoped to a given CONFIG_APP_CENTRAL_ID -- build-only, no flashing.

.DESCRIPTION
    Compiles once into a single shared peripheral/build_incremental
    directory, reused across every node ID (only -DCONFIG_APP_NODE_ID
    changes between builds -- same trick as Sync-And-Build.ps1's
    -SkipPristine, see NOTES.md 2026-08-05/06: ~3min/node with a fresh
    --pristine build_node<N> each time down to ~40s/node once warmed up).
    After each node's build, the resulting zephyr.uf2/.elf/.hex are copied
    out to their own peripheral/build_node<N>/peripheral/zephyr/ -- same
    path every existing flashing instruction (NOTES.md, BUILD_AND_FLASH.md)
    already expects -- so this is a drop-in speed-up, not an interface
    change. The shared build_incremental dir itself is gitignored scratch
    space; only the per-node copies are meant to be flashed from.

    Intended for staging firmware for a whole fleet at once (see NOTES.md
    2026-08-05, CONFIG_APP_CENTRAL_ID multi-rig scoping) so flashing each
    physical board later is just a UF2 drive-copy per BUILD_AND_FLASH.md,
    no build wait.

    Does not touch the central build or flash anything -- build central
    separately, e.g.:
      & $python -m west build --build-dir central\build central --pristine `
          --board xiao_ble/nrf52840 -- -DCONFIG_APP_CENTRAL_ID=<CentralId>

.PARAMETER NodeIds
    Array of node IDs to build (1-100). Use -NodeIds (1..65) for a
    contiguous range, or an explicit list e.g. -NodeIds 8,21,31,32,34.

.PARAMETER CentralId
    CONFIG_APP_CENTRAL_ID to bake into every one of these builds (0-999).
    All node IDs in a single invocation share the same central ID -- run
    the script again with a different -CentralId for a second rig's nodes.

.PARAMETER Board
    Board target passed to `west build --board`. Defaults to
    xiao_ble/nrf52840 (matches every physical board in this project so far).

.EXAMPLE
    ./Build-NodeFleet.ps1 -NodeIds (1..65) -CentralId 1
    # Builds peripheral/build_node1 .. build_node65, all CENTRAL_ID=1.

.EXAMPLE
    ./Build-NodeFleet.ps1 -NodeIds 8,21,31,32,34,35,55,61,63,64 -CentralId 2
    # Re-scope just the currently-deployed set to a different rig's ID.
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 100)]
    [int[]]$NodeIds,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 999)]
    [int]$CentralId,

    [string]$Board = 'xiao_ble/nrf52840'
)

$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$peripheralDir = Join-Path $repoRoot 'peripheral'

Write-Output "Setting up NCS toolchain environment..."
$tc = 'C:\ncs\toolchains\936afb6332'
$env:PATH = "$tc\opt\bin;$tc\opt\bin\Scripts;$tc\mingw64\bin;$tc\bin;$tc\cmd;$tc\usr\bin;" +
            "$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;" +
            "$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_BASE = 'C:\ncs\v3.3.0\zephyr'
$python = "$tc\opt\bin\python.exe"

$uniqueIds = $NodeIds | Sort-Object -Unique
Write-Output "Building $($uniqueIds.Count) node firmware(s) with CONFIG_APP_CENTRAL_ID=$CentralId : $($uniqueIds -join ', ')"

# NOTE: keys are ints, so [ordered]@{} must NOT be used here -- PowerShell's
# ordered-dictionary indexer treats an int key as a positional index into the
# dictionary's current entries (not a hashtable key lookup), which throws
# ArgumentOutOfRangeException on every assignment past the first. Plain
# @{} (unordered hashtable) does real key-based lookup for int keys.
$results = @{}
$logDir = Join-Path $repoRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$batchLog = Join-Path $logDir "build_fleet_central${CentralId}_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"

# Shared, reused across every node ID in this run -- same directory name
# Sync-And-Build.ps1's -SkipPristine uses, already in .gitignore, and lets
# both tools' compiled-object caches benefit each other.
$sharedBuildDir = Join-Path $peripheralDir 'build_incremental'

foreach ($n in $uniqueIds) {
    Write-Output "`n===== Building node $n (central $CentralId) ====="
    $nodeIdArg = "-DCONFIG_APP_NODE_ID=$n"
    $centralIdArg = "-DCONFIG_APP_CENTRAL_ID=$CentralId"

    $out = & $python -m west build --build-dir $sharedBuildDir $peripheralDir --board $Board -- $nodeIdArg $centralIdArg 2>&1
    $ok = $LASTEXITCODE -eq 0

    if ($ok) {
        # Copy this node's result out of the shared dir into its own
        # build_node<N>/peripheral/zephyr/ -- the exact path every existing
        # flashing instruction expects -- before the next node's build
        # overwrites the shared dir's output.
        $srcZephyrDir = Join-Path $sharedBuildDir 'peripheral\zephyr'
        $destZephyrDir = Join-Path $peripheralDir "build_node$n\peripheral\zephyr"
        New-Item -ItemType Directory -Force -Path $destZephyrDir | Out-Null
        foreach ($ext in @('uf2', 'elf', 'hex')) {
            $srcFile = Join-Path $srcZephyrDir "zephyr.$ext"
            if (Test-Path $srcFile) {
                Copy-Item $srcFile -Destination $destZephyrDir -Force
            }
        }
        $ok = Test-Path (Join-Path $destZephyrDir 'zephyr.uf2')
        if (-not $ok) {
            Write-Output "WARNING: node $n build reported success but zephyr.uf2 wasn't found to copy out"
        }
    }

    $results[$n] = $ok

    Add-Content -Path $batchLog -Value "===== node $n ====="
    Add-Content -Path $batchLog -Value $out
    Write-Output ($out | Select-Object -Last 6)
    Write-Output "----- node $n : $(if ($ok) {'OK'} else {'FAILED'}) -----"
}

Write-Output "`n===== SUMMARY (central $CentralId) ====="
$failCount = 0
foreach ($n in $uniqueIds) {
    $status = if ($results[$n]) { 'OK' } else { $failCount++; 'FAILED' }
    Write-Output "node $n : $status"
}
Write-Output "`nFull build log: $batchLog"

if ($failCount -gt 0) {
    Write-Output "`n$failCount build(s) failed -- see $batchLog for details."
    exit 1
}
Write-Output "`nAll $($uniqueIds.Count) builds succeeded."
exit 0
