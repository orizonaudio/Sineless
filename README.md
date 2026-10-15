# Sineless

[![CI on main](https://github.com/orizonaudio/Sineless/actions/workflows/ci.yml/badge.svg?branch=main&event=push)](https://github.com/orizonaudio/Sineless/actions/workflows/ci.yml?query=branch%3Amain+event%3Apush) [![Latest release](https://img.shields.io/github/v/release/orizonaudio/Sineless)](https://github.com/orizonaudio/Sineless/releases/latest) [![Licence: GPL-3.0](https://img.shields.io/github/license/orizonaudio/Sineless)](LICENSE) [![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/orizonaudio/Sineless/badge)](https://scorecard.dev/viewer/?uri=github.com/orizonaudio/Sineless)

An open-source VST3 oscilloscope by Orizon Audio. It shows the audio on its track in real time and passes it through unchanged, with zero latency.

![Sineless's multi-track view, frozen in Beats mode: kick, clap, hi-hat, bass and keys lanes stacked in their track colours](assets/sineless.png)

- Triggered display from 0.5 ms to 1 s, with Auto, Normal, Free and Single trigger modes; spans up to 4 s scroll continuously.
- Tempo-synced sweep: a span in beats or bars, up to 16 bars, aligned to the host's bar lines, with a beat grid and playhead.
- Left, right, mid and side views, overlaid or stacked, on a vertical scale in dB.
- Multi-track view: every track with Sineless inserted, each in its own lane in its track colour and lined up in time, or overlaid. Groups A–D keep separate comparisons apart.
- Freeze, then pan back through the last 4 seconds; optionally freeze automatically when the transport stops.
- Zoom in down to individual samples with ⌘ + scroll (Ctrl on Windows), pinch or drag.
- Time and level cursors that snap to zero crossings and peaks: read the time between them, its frequency and nearest note, beats, and levels in dB.
- Indicative peak, RMS and frequency readouts.
- Built-in help: turn it on, and every control explains itself.
- Settings are saved with your project.

## Requirements

- macOS 13.1 or later, on Apple silicon or Intel.
- Or Windows 10 or later, x64, with the Microsoft Edge WebView2 Runtime. On Windows on Arm, use an x64 host (or REAPER's ARM64EC build, which loads x64 plugins).
- A host that loads VST3 plugins, such as Ableton Live, Cubase, REAPER, Bitwig Studio or Studio One. See [HOSTS.md](HOSTS.md) for the hosts tested so far.

For the multi-track lanes to line up, put Sineless last on each track.

## Install

### macOS

1. Download `Sineless-<version>-macOS.zip` from the releases page and unzip it.
2. Move `Sineless.vst3` to `~/Library/Audio/Plug-Ins/VST3/`.
3. Releases are not signed with an Apple Developer ID yet, so macOS quarantines the download. Clear the quarantine flag in Terminal:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Sineless.vst3
   ```

4. Rescan plugins in your host. Sineless appears under Orizon Audio.

### Windows

1. Download `Sineless-<version>-Windows.zip` from the releases page and unzip it.
2. Move `Sineless.vst3` to `C:\Program Files\Common Files\VST3\`.
3. Rescan plugins in your host. Sineless appears under Orizon Audio.

## Build from source

You need Xcode 26 or later, CMake 3.25 or later, and Node.js 22 (see `.nvmrc`).

```bash
npm ci
cmake --preset dev
cmake --build --preset dev
```

The plugin is built at `build/dev/VST3/Debug/Sineless.vst3`. To work on the interface in a browser, without the plugin, run `npm run dev`; a built-in mock replaces the audio engine.

On Windows, you need Visual Studio 2022 or 2026 with the "Desktop development with C++" workload (the presets use the newest one installed) (add the ARM64 build tools to build for ARM64), Node.js 22 and LLVM (`winget install LLVM.LLVM`, for `clang-format`). In a PowerShell window:

```powershell
npm.cmd ci
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The plugin is built at `build\windows-x64\VST3\Release\Sineless.vst3`; use `windows-arm64` for ARM64, and `windows-x64-debug` or `windows-arm64-debug` for a Debug build to step through. `scripts\check.ps1` runs the Windows checks for both, on the Release build.

`scripts/check.sh` runs every check that CI runs on macOS. It also needs Homebrew LLVM (`brew install llvm`). See [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow.

Without macOS or Windows, the engine and protocol tests still build and run on Linux with Clang and Ninja, as CI's `linux` job does:

```bash
npm ci
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

## Bugs and ideas

Report a bug or suggest a feature in [Issues](https://github.com/orizonaudio/Sineless/issues): for a bug, say which Sineless version, host and operating system, and what you did. Report a security problem privately instead, as [SECURITY.md](SECURITY.md) describes.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
