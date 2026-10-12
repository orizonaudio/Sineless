# Saves a PNG of a window as it appears on screen, in physical pixels.
#
#   Capture-Window.ps1 -Out shot.png -Handle 0x1234                  # a window by handle
#   Capture-Window.ps1 -Out shot.png -Title 'Sineless' [-ProcessId n] # the first visible match
#   Capture-Window.ps1 -Out shot.png                                  # the whole primary screen
#
# The window is brought to the front first. Prints its frame, client size and DPI.
param(
    [Parameter(Mandatory)] [string] $Out,
    [string] $Title,
    [Int64] $Handle = 0,
    [int] $ProcessId = 0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Win32.ps1')
Add-Type -AssemblyName System.Drawing

$window = [IntPtr]$Handle
if ($Title) {
    $ids = if ($ProcessId) { @($ProcessId) } else { @(Get-Process | ForEach-Object Id) }
    $match = Get-ProcessWindows $ids | Where-Object { $_.Visible -and $_.Title -match $Title } |
        Select-Object -First 1
    if (-not $match) { throw "No visible window matches '$Title'." }
    $window = $match.Handle
}

if ($window -ne [IntPtr]::Zero) {
    [Sineless.Win32]::SetForegroundWindow($window) | Out-Null
    Start-Sleep -Milliseconds 400
    $rect = New-Object Sineless.Win32+RECT
    # The visible frame, without the invisible resize borders.
    [Sineless.Win32]::DwmGetWindowAttribute($window, 9, [ref] $rect, 16) | Out-Null
    $client = New-Object Sineless.Win32+RECT
    [Sineless.Win32]::GetClientRect($window, [ref] $client) | Out-Null
    $bounds = [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
    'window 0x{0:X} frame {1}x{2} at {3},{4}; client {5}x{6}; dpi {7}' -f [Int64]$window,
        $bounds.Width, $bounds.Height, $bounds.Left, $bounds.Top, $client.Right, $client.Bottom,
        [Sineless.Win32]::GetDpiForWindow($window)
} else {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
}
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$directory = Split-Path -Parent $Out
if ($directory) { New-Item -ItemType Directory -Force $directory | Out-Null }
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
