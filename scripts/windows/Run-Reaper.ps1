# Runs REAPER unattended: starts it with its own configuration and a ReaScript, answers the
# first-run audio prompt, and waits until the script writes its done file (or REAPER exits, or
# the timeout passes). Needs a licensed REAPER: an evaluation copy's reminder waits for you.
#
#   Run-Reaper.ps1 -Config build\reaper-test -Script scripts\windows\oscope-session.lua `
#                  -Done build\reaper-test\out\done.txt [-Project file.rpp] [-TimeoutSec 900]
#
# The configuration folder holds reaper.ini and is REAPER's resource folder; the user's own
# REAPER settings are not touched. Prints REAPER's process id.
param(
    [string] $Reaper = 'C:\Program Files\REAPER (arm64)\reaper.exe',
    [Parameter(Mandatory)] [string] $Config,
    [Parameter(Mandatory)] [string] $Script,
    [string] $Done,
    [string] $Project,
    [int] $TimeoutSec = 900,
    # How REAPER runs O-Scope: its default (in REAPER's process, if it can) or in a process of its
    # own ("Run as > Dedicated process" in the FX browser).
    [ValidateSet('Default', 'Dedicated')] [string] $RunAs = 'Default',
    [switch] $KeepRunning
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')

$Config = (Resolve-Path $Config).Path
# REAPER keeps "Run as" per plugin file here; 2 is "Dedicated process".
$bridge = Join-Path $Config 'reaper-vstbridge64armwin.ini'
if ($RunAs -eq 'Dedicated') {
    Set-Content -Path $bridge -Value "[vst_dll_options]`r`nO-Scope.vst3=2 -1" -Encoding ASCII
} elseif (Test-Path $bridge) {
    Remove-Item $bridge
}
$arguments = @('-newinst', '-nosplash', '-cfgfile', "`"$Config\reaper.ini`"")
if ($Project) { $arguments += "`"$((Resolve-Path $Project).Path)`"" }
$arguments += "`"$((Resolve-Path $Script).Path)`""
if ($Done -and (Test-Path $Done)) { Remove-Item $Done }

$process = Start-Process -PassThru $Reaper -ArgumentList $arguments
Write-Host "REAPER pid $($process.Id)"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
    foreach ($window in Get-ProcessWindows $process.Id) {
        if (-not $window.Visible) { continue }
        if ($window.Class -eq '#32770' -and $window.Title -eq 'REAPER' -and
                  (Get-WindowText $window.Handle -Children) -match 'not yet selected an audio device') {
            Send-WindowKeys $window.Handle 'n' # The device is set in reaper.ini.
        } elseif ($window.Title -eq 'ReaScript Error') {
            $text = (Get-WindowText $window.Handle -Children) -split "`n" |
                Where-Object { $_ -and $_ -notin 'Close', 'Go to error', 'End script' }
            Write-Warning "ReaScript error: $($text -join ' ')"
            $deadline = Get-Date # Stop waiting; the script has ended.
        }
    }
    if ($Done -and (Test-Path $Done)) { break }
    Start-Sleep -Milliseconds 500
}
if ($Done -and -not (Test-Path $Done)) { Write-Warning "No $Done after $TimeoutSec s." }
if (-not $KeepRunning -and -not $process.HasExited) {
    $process.WaitForExit(30000) | Out-Null
    if (-not $process.HasExited) { Write-Warning 'REAPER did not quit; stopping it.'; $process.Kill() }
}
$process.Id
