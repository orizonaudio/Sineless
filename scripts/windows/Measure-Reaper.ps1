# Appends one line about the running REAPER to a log: private memory, handles, threads, and the
# WebView2 processes Sineless's editors run in (those whose user-data folder is Sineless's).
#
#   Measure-Reaper.ps1 -Out build\reaper-test\sineless-out\measure.txt -Label before
#
# Also lists the Sineless modules REAPER has loaded, with -Modules.
param(
    [Parameter(Mandatory)] [string] $Out,
    [string] $Label = '',
    [switch] $Modules
)
$ErrorActionPreference = 'Stop'
$reaper = Get-Process -Name reaper | Select-Object -First 1
$reaper.Refresh()
$webViews = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" |
    Where-Object { $_.CommandLine -match 'Orizon Audio\\Sineless' })
$line = '{0} {1} private_mb={2:N1} handles={3} threads={4} sineless_webview2={5}' -f (Get-Date -Format 'HH:mm:ss'),
    $Label, ($reaper.PrivateMemorySize64 / 1MB), $reaper.HandleCount, $reaper.Threads.Count, $webViews.Count
# Plugins REAPER runs in processes of their own ("Run as > Dedicated process").
foreach ($bridge in Get-Process -Name 'reaper_host*' -ErrorAction SilentlyContinue) {
    $line += ' | {0}({1}) private_mb={2:N1} handles={3}' -f $bridge.ProcessName, $bridge.Id,
        ($bridge.PrivateMemorySize64 / 1MB), $bridge.HandleCount
}
Add-Content -Path $Out -Value $line
if ($Modules) {
    foreach ($process in @($reaper) + @(Get-Process -Name 'reaper_host*' -ErrorAction SilentlyContinue)) {
        foreach ($module in $process.Modules | Where-Object { $_.FileName -match 'Sineless|WebView2Loader|EmbeddedBrowserWebView' }) {
            Add-Content -Path $Out -Value ("  module in {0} ({1}): {2}" -f $process.ProcessName, $process.Id, $module.FileName)
        }
    }
}
$line
