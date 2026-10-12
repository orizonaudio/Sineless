# Sineless in REAPER on Windows, unattended

Scripts that drive REAPER through a Sineless session without clicks: a ReaScript does the work
inside REAPER, PowerShell starts REAPER, answers its dialogs, takes screenshots and measures.

| Script | Does |
| --- | --- |
| `Test-Reaper.ps1` | The whole run, once per mode (`-Modes Default, Dedicated`). |
| `Test-Scale.ps1` | At the current display scale: `EditorTest.*` (windows-arm64, and windows-x64 with `-X64`), then Sineless's editor in REAPER at its default and minimum size. `-ShotsOnly` skips the tests. |
| `Run-Reaper.ps1` | Starts REAPER with its own configuration and a ReaScript; answers the first-run audio prompt; reports ReaScript errors; waits for the script's done file. `-RunAs Dedicated` runs Sineless in a process of its own. |
| `sineless-session.lua` | The ReaScript. Phases (`SINELESS_PHASE`): `session`, `verify`, `shots`, `keys`; see its header. |
| `Measure-Reaper.ps1` | REAPER's private memory, handles and threads, Sineless's WebView2 processes, and with `-Modules` the Sineless binary REAPER loaded. |
| `Capture-Window.ps1` | A PNG of a window (or the screen) in physical pixels. |
| `Resize-Window.ps1`, `Send-Input.ps1`, `List-Windows.ps1` | Resize a window by dragging its corner; click into a window and type keys; list a process's windows. |
| `Win32.ps1` | Helpers the others dot-source. |

## Setup

REAPER must be licensed: with an evaluation copy, its reminder stops the run until you close it.

1. Build the bundle: `scripts\package.ps1` (lands in `build\release\Sineless.vst3`).
2. Give REAPER a configuration folder of its own, so that yours is not touched: start it once with
   `reaper.exe -newinst -cfgfile C:\dev\Sineless\build\reaper-test\reaper.ini`, accept the audio
   device it proposes, then quit. The folder becomes REAPER's resource folder too.
3. In that `reaper.ini`, under `[reaper]`, add
   `vstpath_winarm64=C:\dev\Sineless\build\release` (the plugin path of REAPER's ARM64EC build),
   and under `[audioconfig]` set the device's rate to 48000 (`waveout_srate=48000` for WaveOut).
4. Run `scripts\windows\Test-Reaper.ps1`. Results land in `build\reaper-results\<mode>\`.
5. For the display scales, build `editor_tests` (`windows-arm64`, and `windows-x64` once), set the
   scale (in a Parallels VM, its display setting overrides Windows': change it there), and run
   `scripts\windows\Test-Scale.ps1` once per scale. Results land in `build\scale-results\<scale>\`.

The session must have the screen to itself: screenshots are of the screen, so keep other windows
out of the way while it runs.

## Notes

- A ReaScript cannot bring a window forward while it waits for PowerShell, so each editor is
  captured right after it opens, while it is the front window.
- Sineless's settings are not host parameters. The script gives each instance its own state by
  editing the JSON in its chunk (`vst_chunk`), with edits of the same length so that the sizes in
  the headers stay right.
- REAPER keeps "Run as" per plugin file in `reaper-vstbridge64armwin.ini`:
  `[vst_dll_options]` / `Sineless.vst3=2 -1` is "Dedicated process".
- REAPER applies a plugin's minimum size only while its window is dragged; a programmatic resize
  skips it. `Resize-Window.ps1` therefore drags, and moves the window clear of the taskbar first.
- An unaware window's pixels are stretched by the system scale: its reported DPI is 96 whatever
  the scale, so click positions in it must be scaled by the system scale.
