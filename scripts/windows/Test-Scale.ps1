# The editor checks at the current display scale: EditorTest.* on windows-arm64 (and windows-x64
# with -X64), then Sineless's editor in REAPER at its default and its minimum size.
#
#   Test-Scale.ps1 [-X64] [-Config build\reaper-test] [-Results build\scale-results]
#
# Build editor_tests for the presets first. Results land in -Results\<scale>\: the test logs,
# the screenshots, and scale.txt. The display scale is a Windows setting: change it by hand
# (Settings > System > Display > Scale) between runs.
param(
    [switch] $X64,
    [switch] $ShotsOnly,
    [string] $Config = 'build\reaper-test',
    [string] $Results = 'build\scale-results'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')
if (-not ('Sineless.WinDpi' -as [type])) {
    Add-Type -Namespace Sineless -Name WinDpi -ReferencedAssemblies System.Drawing -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(System.Drawing.Point point, uint flags);
[DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint x, out uint y);
'@
}
# The primary monitor's effective DPI: the desktop window's can be stale after a live change.
$monitor = [Sineless.WinDpi]::MonitorFromPoint((New-Object System.Drawing.Point 0, 0), 1)
$dpi = 0
$unused = 0
[Sineless.WinDpi]::GetDpiForMonitor($monitor, 0, [ref] $dpi, [ref] $unused) | Out-Null
$scale = [int][math]::Round($dpi * 100 / 96)
$target = Join-Path $Results "$scale"
New-Item -ItemType Directory -Force $target | Out-Null
"display scale $scale% (dpi $dpi), primary screen $([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Size)" |
    Tee-Object (Join-Path $target 'scale.txt')

$presets = @('windows-arm64') + $(if ($X64) { @('windows-x64') } else { @() })
if ($ShotsOnly) { $presets = @() }
foreach ($preset in $presets) {
    $log = Join-Path $target "editor-tests-$preset.txt"
    & "build\$preset\bin\Release\editor_tests.exe" --gtest_filter=EditorTest.* *> $log
    $summary = Select-String -Path $log -Pattern '^\[  (PASSED|FAILED)  \]|^\[  FAILED  \]|display scale' |
        ForEach-Object Line
    "$preset (exit $LASTEXITCODE):"
    $summary | ForEach-Object { "  $_" }
}

$out = Join-Path (Resolve-Path $Config).Path 'sineless-out'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$env:SINELESS_PHASE = 'shots'
$env:SINELESS_PREFIX = "p2-$scale-"
& (Join-Path $PSScriptRoot 'Run-Reaper.ps1') -Config $Config -RunAs Default -Done "$out\done.txt" `
    -Script (Join-Path $PSScriptRoot 'sineless-session.lua') -TimeoutSec 180 | Out-Null
Remove-Item Env:SINELESS_PHASE, Env:SINELESS_PREFIX
Copy-Item "$out\*.png", "$out\shots.log" $target
Select-String -Path "$target\shots.log" -Pattern 'window|FAILED' | ForEach-Object Line
