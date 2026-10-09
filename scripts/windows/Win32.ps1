# Win32 helpers shared by the scripts in this folder. Dot-source it: . .\Win32.ps1
Add-Type -AssemblyName System.Windows.Forms
if (-not ('OScope.Win32' -as [type])) {
    Add-Type -Namespace OScope -Name Win32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wParam, System.Text.StringBuilder lParam);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr window, System.Text.StringBuilder text, int size);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr window, System.Text.StringBuilder text, int size);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out RECT rect, int size);
public delegate bool EnumProc(IntPtr window, IntPtr data);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr data);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr data);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
}
# Per-monitor aware: rectangles and pixels are physical.
[OScope.Win32]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null

function Get-WindowInfo([IntPtr] $window) {
    $text = New-Object System.Text.StringBuilder 512
    $class = New-Object System.Text.StringBuilder 256
    [OScope.Win32]::GetWindowTextW($window, $text, 512) | Out-Null
    [OScope.Win32]::GetClassNameW($window, $class, 256) | Out-Null
    $rect = New-Object OScope.Win32+RECT
    [OScope.Win32]::GetWindowRect($window, [ref] $rect) | Out-Null
    [pscustomobject]@{
        Handle  = $window
        Visible = [OScope.Win32]::IsWindowVisible($window)
        Class   = $class.ToString()
        Title   = $text.ToString()
        Width   = $rect.Right - $rect.Left
        Height  = $rect.Bottom - $rect.Top
    }
}

# The top-level windows of the given processes.
function Get-ProcessWindows([int[]] $ProcessIds) {
    $script:windowList = New-Object System.Collections.ArrayList
    $callback = [OScope.Win32+EnumProc] {
        param($window, $data)
        $owner = 0
        [OScope.Win32]::GetWindowThreadProcessId($window, [ref] $owner) | Out-Null
        if ($ProcessIds -contains $owner) { [void]$script:windowList.Add($window) }
        return $true
    }
    [OScope.Win32]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
    $script:windowList | ForEach-Object { Get-WindowInfo $_ }
}

# The window's title, or with -Children the titles of all its descendants (a dialog's text).
function Get-WindowText([IntPtr] $Window, [switch] $Children) {
    if (-not $Children) { return (Get-WindowInfo $Window).Title }
    $script:childText = New-Object System.Collections.ArrayList
    $callback = [OScope.Win32+EnumProc] {
        param($child, $data)
        # WM_GETTEXT: GetWindowText cannot read another process's edit controls.
        $text = New-Object System.Text.StringBuilder 8192
        [OScope.Win32]::SendMessageW($child, 0x000D, [IntPtr]8192, $text) | Out-Null
        [void]$script:childText.Add($text.ToString())
        return $true
    }
    [OScope.Win32]::EnumChildWindows($Window, $callback, [IntPtr]::Zero) | Out-Null
    $script:childText -join "`n"
}

# Brings the window to the front and types keys (System.Windows.Forms.SendKeys syntax).
function Send-WindowKeys([IntPtr] $Window, [string] $Keys) {
    [OScope.Win32]::SetForegroundWindow($Window) | Out-Null
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
}
