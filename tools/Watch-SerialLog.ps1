<#
.SYNOPSIS
    Reads a serial (COM) port and writes every line to a timestamped log file,
    echoing to the console at the same time.

.DESCRIPTION
    Used to capture boot/runtime logs from the central or peripheral XIAO
    nRF52840 boards. Each line received on the port is prefixed with a
    wall-clock timestamp and appended to the log file, so log lines can be
    correlated across the two boards (their own app-level printk output has
    no timestamps).

.PARAMETER Port
    COM port name, e.g. "COM140". If omitted, the script lists available
    ports and exits.

.PARAMETER OutFile
    Path to the log file to write. Defaults to
    logs/<Port>_<yyyyMMdd_HHmmss>.log under the repo root.

.PARAMETER BaudRate
    Serial baud rate. Defaults to 115200 (matches the board's console UART).

.PARAMETER DurationSeconds
    If set, the script stops automatically after this many seconds. If
    omitted, it runs until interrupted (Ctrl+C).

.EXAMPLE
    ./Watch-SerialLog.ps1 -Port COM140
    ./Watch-SerialLog.ps1 -Port COM140 -OutFile logs/central_run1.log -DurationSeconds 120
#>
param(
    [string]$Port,
    [string]$OutFile,
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 0
)

$ErrorActionPreference = 'Stop'

if (-not $Port) {
    Write-Output "Available serial ports:"
    Get-CimInstance -ClassName Win32_PnPEntity |
        Where-Object { $_.Name -match 'COM\d+' } |
        Select-Object Name |
        ForEach-Object { Write-Output "  $($_.Name)" }
    Write-Output "`nUsage: ./Watch-SerialLog.ps1 -Port COM<n> [-OutFile path] [-DurationSeconds n]"
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutFile) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $logsDir = Join-Path $repoRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
    $OutFile = Join-Path $logsDir "$($Port)_$stamp.log"
} elseif (-not [System.IO.Path]::IsPathRooted($OutFile)) {
    $OutFile = Join-Path $repoRoot $OutFile
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null

$sp = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 500
$sp.NewLine = "`n"

try {
    $sp.Open()
} catch {
    Write-Error "Failed to open $Port : $($_.Exception.Message)"
    exit 1
}

Write-Output "Logging $Port -> $OutFile (Ctrl+C to stop)"
$writer = [System.IO.StreamWriter]::new($OutFile, $true)
$writer.AutoFlush = $true

$startTime = Get-Date
try {
    while ($true) {
        if ($DurationSeconds -gt 0 -and ((Get-Date) - $startTime).TotalSeconds -ge $DurationSeconds) {
            break
        }
        try {
            $line = $sp.ReadLine()
        } catch [System.TimeoutException] {
            continue
        } catch {
            # Anything other than a read timeout (port unexpectedly closed,
            # USB hiccup, etc.) used to fall through uncaught and silently
            # end the whole capture early via the try/finally below -- no
            # error text, just a short log with a normal-looking "Log saved"
            # footer, which made a truncated capture look successful. Try to
            # recover instead: close and reopen the port, then keep going
            # until the requested duration actually elapses.
            $ts = Get-Date -Format 'HH:mm:ss.fff'
            $note = "[$ts] *** read error: $($_.Exception.GetType().Name): $($_.Exception.Message) -- reopening port ***"
            Write-Output $note
            $writer.WriteLine($note)
            try { if ($sp.IsOpen) { $sp.Close() } } catch {}
            Start-Sleep -Milliseconds 500
            try {
                $sp.Open()
            } catch {
                $failMsg = "[$ts] *** could not reopen $Port : $($_.Exception.Message) -- stopping capture ***"
                Write-Output $failMsg
                $writer.WriteLine($failMsg)
                break
            }
            continue
        }
        $ts = Get-Date -Format 'HH:mm:ss.fff'
        $entry = "[$ts] $line"
        Write-Output $entry
        $writer.WriteLine($entry)
    }
} finally {
    $writer.Close()
    if ($sp.IsOpen) { $sp.Close() }
    $elapsed = ((Get-Date) - $startTime).TotalSeconds
    Write-Output "`nLog saved to $OutFile ($([math]::Round($elapsed))s elapsed of $DurationSeconds requested)"
}
