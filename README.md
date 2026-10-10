# O-Scope

An open-source VST3 oscilloscope by Orizon Audio. It shows the audio on its track in real time and passes it through unchanged, with zero latency.

![O-Scope's multi-track view, frozen in Beats mode: kick, clap, hi-hat, bass and keys lanes stacked in their track colours](assets/o-scope.png)

- Triggered display from 0.5 ms to 1 s, with Auto, Normal, Free and Single trigger modes; spans up to 4 s scroll continuously.
- Tempo-synced sweep: a span in beats or bars, up to 16 bars, aligned to the host's bar lines, with a beat grid and playhead.
- Left, right, mid and side views, overlaid or stacked, on a vertical scale in dB.
- Multi-track view: every track with O-Scope inserted, each in its own lane in its track colour and lined up in time, or overlaid. Groups A–D keep separate comparisons apart.
- Freeze, then pan back through the last 4 seconds; optionally freeze automatically when the transport stops.
- Zoom in down to individual samples with ⌘ + scroll (Ctrl on Windows), pinch or drag.
- Time and level cursors that snap to zero crossings and peaks: read the time between them, its frequency and nearest note, beats, and levels in dB.
- Indicative peak, RMS and frequency readouts.
- Built-in help: turn it on, and every control explains itself.
- Settings are saved with your project.

## Requirements

- macOS 13.1 or later, on Apple silicon or Intel.
- Or Windows 10 or later, x64 or ARM64, with the Microsoft Edge WebView2 Runtime.
- A host that loads VST3 plugins, such as Ableton Live, Cubase, REAPER, Bitwig Studio or Studio One. See [HOSTS.md](HOSTS.md) for the hosts tested so far.

For the multi-track lanes to line up, put O-Scope last on each track.

## Install

### macOS

1. Download `O-Scope-<version>-macOS.zip` from the releases page and unzip it.
2. Move `O-Scope.vst3` to `~/Library/Audio/Plug-Ins/VST3/`.
3. Releases are not signed with an Apple Developer ID yet, so macOS quarantines the download. Clear the quarantine flag in Terminal:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/O-Scope.vst3
   ```

4. Rescan plugins in your host. O-Scope appears under Orizon Audio.

### Windows

1. Download `O-Scope-<version>-Windows.zip` from the releases page and unzip it.
2. Move `O-Scope.vst3` to `C:\Program Files\Common Files\VST3\`.
3. Rescan plugins in your host. O-Scope appears under Orizon Audio.

## Build from source

You need Xcode 26 or later, CMake 3.25 or later, and Node.js 22 (see `.nvmrc`).

```bash
npm ci
cmake --preset dev
cmake --build --preset dev
```

The plugin is built at `build/dev/VST3/Debug/O-Scope.vst3`. To work on the interface in a browser, without the plugin, run `npm run dev`; a built-in mock replaces the audio engine.

On Windows, you need Visual Studio 2022 with the "Desktop development with C++" workload (add the ARM64 build tools to build for ARM64), Node.js 22 and LLVM (`winget install LLVM.LLVM`, for `clang-format`). In a PowerShell window:

```powershell
npm.cmd ci
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The plugin is built at `build\windows-x64\VST3\Debug\O-Scope.vst3`; use `windows-arm64` for ARM64. `scripts\check.ps1` runs the Windows checks for both.

`scripts/check.sh` runs every check that CI runs. It also needs Homebrew LLVM (`brew install llvm`). See [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
