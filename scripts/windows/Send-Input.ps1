# Clicks into the first visible window whose title matches, then types keys there, as a user would
# (synthesised input, System.Windows.Forms.SendKeys syntax for the keys).
#
#   Send-Input.ps1 -Title 'O-Scope.*Track 1' -ClickX 45 -ClickY 70 -Keys ' '
#
# The click point is in device-independent pixels from the window frame's top-left corner, scaled
# by the window's DPI. Run it without waiting for it from the window's own thread.
param(
    [Parameter(Mandatory)] [string] $Title,
    [int] $ClickX = -1,
    [int] $ClickY = -1,
    [string] $Keys = '',
    [int] $DelayMs = 500,
    [string] $Log
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')
if (-not ('OScope.WinMouse' -as [type])) {
    Add-Type -Namespace OScope -Name WinMouse -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
'@
}
$window = Get-ProcessWindows @(Get-Process | ForEach-Object Id) |
    Where-Object { $_.Visible -and $_.Title -match $Title } | Select-Object -First 1
if (-not $window) { throw "No visible window matches '$Title'." }
[OScope.Win32]::SetForegroundWindow($window.Handle) | Out-Null
Start-Sleep -Milliseconds 300
if ($ClickX -ge 0) {
    $rect = New-Object OScope.Win32+RECT
    [OScope.Win32]::DwmGetWindowAttribute($window.Handle, 9, [ref] $rect, 16) | Out-Null
    $scale = [OScope.Win32]::GetDpiForWindow($window.Handle) / 96
    [OScope.WinMouse]::SetCursorPos($rect.Left + [int]($ClickX * $scale), $rect.Top + [int]($ClickY * $scale)) | Out-Null
    Start-Sleep -Milliseconds 200
    [OScope.WinMouse]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [OScope.WinMouse]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $DelayMs
}
if ($Log) {
    # The window with keyboard focus when the keys go out.
    if (-not ('OScope.WinFocus' -as [type])) {
        Add-Type -Namespace OScope -Name WinFocus -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
  public int cbSize; public int flags; public IntPtr hwndActive; public IntPtr hwndFocus;
  public IntPtr hwndCapture; public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize;
  public IntPtr hwndCaret; public RECT rcCaret; }
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
'@
    }
    $foreground = [OScope.WinFocus]::GetForegroundWindow()
    $process = 0
    $info = New-Object OScope.WinFocus+GUITHREADINFO
    $info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
    [OScope.WinFocus]::GetGUIThreadInfo([OScope.WinFocus]::GetWindowThreadProcessId($foreground, [ref] $process), [ref] $info) | Out-Null
    $focus = if ($info.hwndFocus -ne [IntPtr]::Zero) { (Get-WindowInfo $info.hwndFocus).Class } else { 'none' }
}
if ($Keys) { [System.Windows.Forms.SendKeys]::SendWait($Keys) }
if ($Log) {
    Add-Content $Log ("{0} sent to 0x{1:X} {2}; foreground '{3}', focus [{4}]" -f (Get-Date -Format HH:mm:ss.fff),
        [Int64]$window.Handle, $window.Title, (Get-WindowInfo $foreground).Title, $focus)
}
