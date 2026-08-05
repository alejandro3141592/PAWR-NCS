<#
.SYNOPSIS
    One-shot sync/build/flash/monitor pipeline for the central (or peripheral)
    app: fetch + check for the other person's changes, commit + push your own,
    rebuild (pristine by default), flash over UF2 bootloader, capture a
    serial log, then push the log (and NOTES.md) back so the other person
    sees it too.

.DESCRIPTION
    Steps, in order:
      1. git fetch, warn if the remote has commits you don't have locally
         (does NOT auto-merge/rebase -- you decide how to reconcile that).
      2. If the working tree has changes, stage + commit them with an
         auto-generated message (or -CommitMessage if given), then push.
      3. `west build` for the selected app/board -- pristine (full clean
         rebuild) by default, or incremental with -SkipPristine.
      4. Play a sound so you know the (slow) build finished.
      5. Wait for the board's UF2 bootloader drive to appear, copy the
         firmware over to flash it.
      6. Wait for the console COM port to re-enumerate, capture N seconds of
         boot log via Watch-SerialLog.ps1.
      7. Commit + push the new log file (and NOTES.md, if it has uncommitted
         changes) so the other person picks it up on their next pull.

.PARAMETER App
    "central" or "peripheral". Selects the app dir, board, and default
    build-dir.

.PARAMETER Board
    Board target passed to `west build --board`. Defaults to
    xiao_ble/nrf52840 (both boards in this project use the same target).

.PARAMETER CommitMessage
    Commit message to use if there are local changes to commit. Defaults to
    an auto-generated one listing changed files.

.PARAMETER MonitorSeconds
    How long to capture the post-flash serial log for. Default 60.

.PARAMETER SkipGit
    Skip all git steps (fetch/commit/push) -- just build/flash/monitor.

.PARAMETER SkipFlash
    Build only; don't wait for/flash the bootloader drive or monitor.

.PARAMETER SkipPristine
    Skip `--pristine` for a much faster incremental build -- seconds instead
    of minutes, since only the files actually affected by your change get
    recompiled instead of everything.

    For -App peripheral, this also switches the build directory to a single
    shared peripheral/build_incremental (instead of the normal per-node
    build_node<N>), reused across every -NodeId -- that's what actually
    makes flashing many distinct boards fast: compiled Zephyr/nrfxlib
    object files carry over from the previous board, only main.c (which
    CONFIG_APP_NODE_ID feeds into) gets recompiled and relinked each time.
    A fresh build_node<N> per ID would defeat the point, since there'd be
    nothing to reuse in a directory nothing has ever been built into. This
    means -SkipPristine builds and normal pristine builds for the same
    -NodeId live in different directories -- switching back to a pristine
    build after using -SkipPristine (or vice versa) still does a full
    rebuild that one time, then stays fast within whichever mode you stick
    with.

    Safe for changes that only touch CONFIG_APP_NODE_ID or other simple
    Kconfig/source-only edits.

    Do NOT use after touching devicetree files (boards/*.overlay),
    CMakeLists.txt, or Kconfig.sysbuild -- incremental builds have
    previously and repeatedly failed to pick up changes to those on this
    toolchain (see Summary.md/NOTES.md gotchas), silently flashing stale
    firmware. When in doubt, omit this flag and let it run pristine (the
    default).

.PARAMETER NodeId
    Peripheral only: overrides CONFIG_APP_NODE_ID (1-100) for this build via
    -DCONFIG_APP_NODE_ID=N, so a physical board's reported node_id doesn't
    have to match whatever is checked into peripheral/Kconfig's default.
    Also switches the build directory to peripheral/build_node<N> so builds
    for different physical boards don't clobber each other's incremental
    build cache. Ignored for -App central.

    If omitted, falls back to the contents of peripheral/node_id.txt (a
    single integer, gitignored -- each physical board's own local file, not
    synced between machines). If that file is also missing or empty, no
    override is applied and Kconfig's own default (1) is used.

.EXAMPLE
    ./Sync-And-Build.ps1 -App central
    ./Sync-And-Build.ps1 -App peripheral -MonitorSeconds 120
    ./Sync-And-Build.ps1 -App peripheral -NodeId 2 -SkipGit
    # or just edit peripheral/node_id.txt once, then:
    ./Sync-And-Build.ps1 -App peripheral
    # flashing many boards, each just a different node ID -- fast path:
    ./Sync-And-Build.ps1 -App peripheral -NodeId 7 -SkipPristine
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('central', 'peripheral')]
    [string]$App,

    [string]$Board = 'xiao_ble/nrf52840',
    [string]$CommitMessage,
    [int]$MonitorSeconds = 60,
    [ValidateRange(1, 100)]
    [int]$NodeId = 0,
    [switch]$SkipGit,
    [switch]$SkipFlash,
    [switch]$SkipPristine
)

if ($App -eq 'peripheral' -and $NodeId -eq 0) {
    $nodeIdFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'peripheral\node_id.txt'
    if (Test-Path $nodeIdFile) {
        $fileContent = (Get-Content $nodeIdFile -Raw -ErrorAction SilentlyContinue).Trim()
        $parsedNodeId = 0
        if ([int]::TryParse($fileContent, [ref]$parsedNodeId) -and $parsedNodeId -ge 1 -and $parsedNodeId -le 100) {
            $NodeId = $parsedNodeId
            Write-Output "Using CONFIG_APP_NODE_ID=$NodeId from peripheral\node_id.txt"
        } elseif ($fileContent) {
            Write-Output "WARNING: peripheral\node_id.txt contains '$fileContent', not a valid node ID (1-100) -- ignoring, using Kconfig default."
        }
    }
}


# Deliberately NOT 'Stop': native tools (git, west/python) routinely write
# informational output to stderr, which PowerShell 5.1 wraps as a
# NativeCommandError under 2>&1 redirection -- with ErrorActionPreference
# 'Stop' that becomes a terminating error even on exit code 0, killing the
# script on the very first stderr line a native command prints. Exit codes
# are checked explicitly after each step that matters instead.
$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot $App
if ($App -eq 'peripheral' -and $SkipPristine) {
    # One shared directory reused across every -NodeId, not build_node<N>
    # per ID -- the whole point of -SkipPristine is to carry compiled
    # Zephyr/nrfxlib object files over between boards. A fresh per-node
    # directory would defeat that (nothing to reuse in a directory that's
    # never been built into before), so this intentionally diverges from
    # the pristine path's per-node isolation.
    $buildDir = Join-Path $appDir 'build_incremental'
} elseif ($App -eq 'peripheral' -and $NodeId -gt 0) {
    $buildDir = Join-Path $appDir "build_node$NodeId"
} else {
    $buildDir = Join-Path $appDir 'build'
}

function Write-Step {
    param([string]$Text)
    Write-Output "`n==> $Text"
}

function Play-Done {
    param([string]$SoundName = 'Asterisk')
    try {
        [System.Media.SystemSounds]::$SoundName.Play()
        Start-Sleep -Milliseconds 400
        [System.Media.SystemSounds]::$SoundName.Play()
    } catch {
        Write-Output "`a"  # terminal bell fallback
    }
}

function Play-Error {
    try {
        [System.Media.SystemSounds]::Hand.Play()
    } catch {
        Write-Output "`a`a`a"
    }
}

# ----------------------------------------------------------------------
# 1-2. Git: fetch, warn on remote drift, commit + push local changes
# ----------------------------------------------------------------------
if (-not $SkipGit) {
    Write-Step "Fetching remote to check for the other person's changes..."
    Set-Location $repoRoot
    git fetch origin 2>&1 | ForEach-Object { Write-Output $_ }

    $behind = git rev-list --count 'HEAD..origin/main' 2>$null
    if ($behind -and [int]$behind -gt 0) {
        Write-Output "NOTE: origin/main has $behind commit(s) you don't have locally yet."
        Write-Output "      Run 'git pull' / 'git merge origin/main' yourself before or after this build --"
        Write-Output "      this script does not auto-merge, to avoid silently resolving conflicts for you."
    }

    $dirty = git status --porcelain 2>$null
    if ($dirty) {
        Write-Step "Committing local changes..."
        git add -A
        if (-not $CommitMessage) {
            $changedFiles = (git diff --cached --name-only) -join ', '
            $CommitMessage = "Update $changedFiles"
        }
        git commit -m $CommitMessage
        Write-Step "Pushing..."
        git push origin main
        if ($LASTEXITCODE -ne 0) {
            Write-Output "git push failed (exit $LASTEXITCODE) -- likely the remote has commits you don't have (see NOTE above). Resolve manually (git pull) and re-run."
            Play-Error
            exit 1
        }
    } else {
        Write-Output "Working tree already clean, nothing to commit."
    }
}

# ----------------------------------------------------------------------
# 3. west build (pristine unless -SkipPristine)
# ----------------------------------------------------------------------
Write-Step "Setting up NCS toolchain environment..."
$tc = 'C:\ncs\toolchains\936afb6332'
$env:PATH = "$tc\opt\bin;$tc\opt\bin\Scripts;$tc\mingw64\bin;$tc\bin;$tc\cmd;$tc\usr\bin;" +
            "$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;" +
            "$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_BASE = 'C:\ncs\v3.3.0\zephyr'
$python = "$tc\opt\bin\python.exe"

$pristineLabel = if ($SkipPristine) { "incremental" } else { "pristine" }
Write-Step "Running $pristineLabel west build for $App (board: $Board)..."
$buildLogPrefix = if ($App -eq 'peripheral' -and $NodeId -gt 0) { "${App}_node${NodeId}" } else { $App }
$buildLog = Join-Path $repoRoot "logs\${buildLogPrefix}_build_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $buildLog) | Out-Null

$buildArgs = @('-m', 'west', 'build', '--build-dir', $buildDir, $appDir)
if (-not $SkipPristine) {
    $buildArgs += '--pristine'
}
$buildArgs += @('--board', $Board, '--', '-DDEBUG_THREAD_INFO=Off', "-D${App}_DEBUG_THREAD_INFO=Off")
if ($App -eq 'peripheral' -and $NodeId -gt 0) {
    $buildArgs += "-DCONFIG_APP_NODE_ID=$NodeId"
}

& $python @buildArgs 2>&1 | Tee-Object -FilePath $buildLog | ForEach-Object { Write-Output $_ }
$buildExit = $LASTEXITCODE

# ----------------------------------------------------------------------
# 4. Audible signal
# ----------------------------------------------------------------------
if ($buildExit -eq 0) {
    Write-Step "Build succeeded."
    Play-Done
} else {
    Write-Step "BUILD FAILED (exit $buildExit) -- see $buildLog"
    Play-Error
    exit $buildExit
}

if ($SkipFlash) {
    Write-Output "`n-SkipFlash set, stopping after build."
    exit 0
}

# ----------------------------------------------------------------------
# 5. Wait for bootloader drive, flash
# ----------------------------------------------------------------------
Write-Step "Put the $App board into UF2 bootloader mode (double-tap reset) now."
Write-Output "Waiting up to 60s for the XIAO-BOOT drive to appear..."

$bootDrive = $null
$waited = 0
while ($waited -lt 60) {
    $vol = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq 'XIAO-BOOT' }
    if ($vol) { $bootDrive = $vol; break }
    Start-Sleep -Seconds 2
    $waited += 2
}

if (-not $bootDrive) {
    Write-Output "Timed out waiting for XIAO-BOOT drive -- skipping flash. Build artifacts are still in $buildDir."
    Play-Error
    exit 1
}

$uf2Path = Join-Path $buildDir "$App\zephyr\zephyr.uf2"
if (-not (Test-Path $uf2Path)) {
    Write-Output "ERROR: expected firmware not found at $uf2Path"
    Play-Error
    exit 1
}

function Get-ComPorts {
    Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'COM\d+' } |
        ForEach-Object { [regex]::Match($_.Name, 'COM\d+').Value }
}

# Snapshot COM ports before flashing so the newly-appeared one (this
# board's console re-enumerating after reset) can be identified even if
# another board is already connected on a different port.
$portsBefore = @(Get-ComPorts)

Write-Step "Flashing $uf2Path to $($bootDrive.DriveLetter):\..."
Copy-Item $uf2Path -Destination "$($bootDrive.DriveLetter):\firmware.uf2"
Write-Output "Copied. Board should reboot into the new firmware momentarily."
Start-Sleep -Seconds 3

# ----------------------------------------------------------------------
# 6. Monitor: wait for the console COM port, capture a log
# ----------------------------------------------------------------------
Write-Step "Waiting for the console serial port to re-enumerate..."
$comPort = $null
$waited = 0
while ($waited -lt 30) {
    $portsNow = @(Get-ComPorts)
    $newPorts = @($portsNow | Where-Object { $_ -notin $portsBefore })
    if ($newPorts.Count -eq 1) {
        $comPort = $newPorts[0]
        break
    } elseif ($newPorts.Count -gt 1) {
        Write-Output "Multiple new ports appeared ($($newPorts -join ', ')) -- can't tell which is this board. Pick one manually with Watch-SerialLog.ps1."
        break
    }
    Start-Sleep -Seconds 2
    $waited += 2
}

if (-not $comPort) {
    Write-Output "Could not identify the board's serial port after flashing -- skipping monitor step."
    Play-Error
    exit 1
}

# The port can enumerate before the app's own USB-CDC console is actually
# ready to transmit (seen repeatedly: capture completes with 0 bytes, but a
# manual reset afterward immediately shows output). Give it more settle time
# than the port-detection loop above already used.
Start-Sleep -Seconds 5

$logNamePrefix = if ($App -eq 'peripheral' -and $NodeId -gt 0) { "${App}_node${NodeId}" } else { $App }
$logFile = Join-Path $repoRoot "logs\${logNamePrefix}_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"
Write-Step "Capturing $MonitorSeconds s of $comPort output to $logFile ..."
# Watch-SerialLog.ps1 sets its own ErrorActionPreference='Stop' and can
# Write-Error (e.g. port already open elsewhere) -- that becomes a
# terminating exception that would otherwise unwind straight through this
# call and kill the rest of this script too. Catch it so a monitor failure
# is reported but doesn't prevent finishing up (e.g. nothing to push, but
# at least say so instead of silently stopping).
$monitorFailed = $false
try {
    & (Join-Path $PSScriptRoot 'Watch-SerialLog.ps1') -Port $comPort -OutFile $logFile -DurationSeconds $MonitorSeconds
} catch {
    Write-Output "Monitor step failed: $($_.Exception.Message)"
    Write-Output "(Often means something else -- e.g. a VS Code serial monitor tab -- already has $comPort open. Close it and re-run, or use tools/Watch-SerialLog.ps1 manually once free.)"
    $monitorFailed = $true
}

if (-not $monitorFailed -and (Test-Path $logFile) -and (Get-Item $logFile).Length -eq 0) {
    Write-Output "Captured log is empty -- the board likely wasn't producing output yet when capture started (this has happened before; a manual reset after flashing tends to fix it). Not pushing an empty file."
    Remove-Item $logFile
    $monitorFailed = $true
}

# ----------------------------------------------------------------------
# 7. Push logs + NOTES.md so the other person can see them
# ----------------------------------------------------------------------
if (-not $SkipGit) {
    Write-Step "Pushing log + notes..."
    if ($monitorFailed -and -not (Test-Path $logFile)) {
        Write-Output "No log file to push (monitor step didn't produce one)."
    }
    Set-Location $repoRoot
    git add $logFile
    $notesPath = Join-Path $repoRoot 'NOTES.md'
    if ((git status --porcelain $notesPath 2>$null)) {
        git add $notesPath
    }
    $dirty = git status --porcelain 2>$null
    if ($dirty) {
        git commit -m "Add $App run log $(Split-Path -Leaf $logFile)"
        git push origin main
        Write-Output "Log pushed."
    } else {
        Write-Output "Nothing new to push (log already tracked/unchanged?)."
    }
}

Play-Done
Write-Step "Done."
