# Lists a process's top-level windows: handle, visibility, class, title and size.
#
#   List-Windows.ps1 -ProcessName reaper
param([string] $ProcessName = 'reaper')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')
$ids = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | ForEach-Object Id)
foreach ($window in Get-ProcessWindows $ids) {
    '0x{0:X} {1} [{2}] "{3}" {4}x{5}' -f [Int64]$window.Handle,
        $(if ($window.Visible) { 'visible' } else { 'hidden ' }), $window.Class, $window.Title,
        $window.Width, $window.Height
}
