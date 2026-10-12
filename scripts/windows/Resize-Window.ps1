# Resizes the first visible window whose title matches by dragging its bottom-right corner with
# the mouse, as a user would, so that the window's owner sees an ordinary resize.
#
#   Resize-Window.ps1 -Title 'Sineless.*Track 1' -Width 1200 -Height 900   # frame size, physical px
#   Resize-Window.ps1 -Title 'Sineless.*Track 1' -ShrinkHeight 250         # by 250 DIPs, at its DPI
#
# Run it without waiting for it from the window's own thread (ExecProcess with -1 in a ReaScript):
# that thread has to handle the drag.
param(
    [Parameter(Mandatory)] [string] $Title,
    [int] $Width = 0,
    [int] $Height = 0,
    [int] $ShrinkWidth = 0,
    [int] $ShrinkHeight = 0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')
if (-not ('Sineless.WinMouse' -as [type])) {
    Add-Type -Namespace Sineless -Name WinMouse -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
'@
}
$window = Get-ProcessWindows @(Get-Process | ForEach-Object Id) |
    Where-Object { $_.Visible -and $_.Title -match $Title } | Select-Object -First 1
if (-not $window) { throw "No visible window matches '$Title'." }
[Sineless.Win32]::SetForegroundWindow($window.Handle) | Out-Null
Start-Sleep -Milliseconds 300
$rect = New-Object Sineless.Win32+RECT
[Sineless.Win32]::DwmGetWindowAttribute($window.Handle, 9, [ref] $rect, 16) | Out-Null
$scale = [Sineless.Win32]::GetDpiForWindow($window.Handle) / 96
if ($Width -le 0) { $Width = $rect.Right - $rect.Left - [int]($ShrinkWidth * $scale) }
if ($Height -le 0) { $Height = $rect.Bottom - $rect.Top - [int]($ShrinkHeight * $scale) }

# The corner must be inside the work area (not under the taskbar) to be grabbed.
$workArea = [System.Windows.Forms.Screen]::FromHandle($window.Handle).WorkingArea
if ($rect.Bottom + 8 -gt $workArea.Bottom) {
    if (-not ('Sineless.WinMove' -as [type])) {
        Add-Type -Namespace Sineless -Name WinMove -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
'@
    }
    $top = [math]::Max($workArea.Top, $workArea.Bottom - ($rect.Bottom - $rect.Top) - 16)
    # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
    [Sineless.WinMove]::SetWindowPos($window.Handle, [IntPtr]::Zero, $rect.Left, $top, 0, 0, 0x4015) | Out-Null
    Start-Sleep -Milliseconds 800
    [Sineless.Win32]::DwmGetWindowAttribute($window.Handle, 9, [ref] $rect, 16) | Out-Null
}

# Grabbed in the resize border, just outside the visible frame's corner.
$from = @(($rect.Right + 4), ($rect.Bottom + 4))
$to = @(($rect.Left + $Width + 4), ($rect.Top + $Height + 4))
[Sineless.WinMouse]::SetCursorPos($from[0], $from[1]) | Out-Null
Start-Sleep -Milliseconds 200
[Sineless.WinMouse]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) # Left button down.
$steps = 30
for ($step = 1; $step -le $steps; ++$step) {
    $x = $from[0] + ($to[0] - $from[0]) * $step / $steps
    $y = $from[1] + ($to[1] - $from[1]) * $step / $steps
    [Sineless.WinMouse]::SetCursorPos([int]$x, [int]$y) | Out-Null
    Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 500 # Let a busy window catch up before the button goes up.
[Sineless.WinMouse]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) # Left button up.
Start-Sleep -Milliseconds 500
[Sineless.Win32]::DwmGetWindowAttribute($window.Handle, 9, [ref] $rect, 16) | Out-Null
'frame now {0}x{1} (asked {2}x{3})' -f ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top), $Width, $Height
