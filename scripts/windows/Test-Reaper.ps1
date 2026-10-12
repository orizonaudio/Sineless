# Sineless in REAPER, unattended: the session (4 tracks, editors, open/close cycles while playing,
# save), then a fresh REAPER that reopens the project and checks each Sineless's state; once with
# Sineless as REAPER runs it by default, once in a dedicated process.
#
#   Test-Reaper.ps1 [-Config build\reaper-test] [-Results <folder>] [-Cycles 50]
#
# The configuration folder must hold a reaper.ini with an audio device and
# vstpath_winarm64=<folder with Sineless.vst3> (see README.md here). Results (logs, measurements,
# projects and screenshots) land in -Results, one subfolder per mode.
param(
    [string] $Config = 'build\reaper-test',
    [string] $Results = 'build\reaper-results',
    [int] $Cycles = 50,
    [string[]] $Modes = @('Default', 'Dedicated')
)
$ErrorActionPreference = 'Stop'
$out = Join-Path (Resolve-Path $Config).Path 'sineless-out'
$lua = Join-Path $PSScriptRoot 'sineless-session.lua'
$timeout = 300 + $Cycles * 10

foreach ($mode in $Modes) {
    Write-Host "`n==> $mode"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    $env:SINELESS_CYCLES = $Cycles
    $env:SINELESS_PHASE = 'session'
    & (Join-Path $PSScriptRoot 'Run-Reaper.ps1') -Config $Config -Script $lua -RunAs $mode `
        -Done "$out\done.txt" -TimeoutSec $timeout | Out-Host
    $env:SINELESS_PHASE = 'verify'
    & (Join-Path $PSScriptRoot 'Run-Reaper.ps1') -Config $Config -Script $lua -RunAs $mode `
        -Project "$out\session.rpp" -Done "$out\done.txt" -TimeoutSec 180 | Out-Host
    Remove-Item Env:SINELESS_PHASE, Env:SINELESS_CYCLES

    $target = Join-Path $Results $mode.ToLower()
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item "$out\*" $target -Recurse
    Get-Content "$target\measure.txt", "$target\verify.txt" -ErrorAction SilentlyContinue | Out-Host
}
