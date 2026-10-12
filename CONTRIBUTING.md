# Contributing

## Definition of Done

Every change:

- [ ] `scripts/check.sh` passes locally. CI runs the same script, on demand. A change to Windows code, or to anything built on Windows, also passes `scripts\check.ps1` on Windows.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Any new dependency was agreed on first and is pinned.
- [ ] A number the engine and the UI both use (a limit, a threshold, an interval, a layout size) is defined once in `protocol/src/limits.ts`, which the C++ header is generated from, and help text reads it from there too.

## Performance

`scripts/performance.sh` (macOS) and `scripts\performance.ps1` (Windows) enforce performance limits, measured on optimised builds of two benchmarks. The check scripts run them unless `SKIP_PERFORMANCE=1`; CI runs them as a separate job that reports but does not block, since shared CI machines are too noisy for hard limits.

- `process_bench`: real processor instances driven through the VST3 API at every common block size and sample rate. Eight instances together must stay within 5% of each block's deadline at p99.9. `scripts\performance.ps1` runs it on GitHub's Windows machines, or anywhere with `RUN_BENCH=1`.
- `perf_bench`: the real editor page in a WKWebView, with tracks playing in real time. Seven tracks over four bars, and fifteen tracks over sixteen bars at 60 BPM and 96 kHz (a 64 s sweep), must draw at 45 frames/s or more with the analysis thread under its budget, and opening and closing the editor 50 times must leave nothing behind.

To compare changes, `scripts/bench.sh` prints CPU by part, frames/s, memory and data per frame for a fixed set of scenarios, at 48 kHz and 120 BPM unless a scenario says otherwise (`perf_bench` takes `--rate` and `--tempo`). CPU in % of a core varies with system load, so compare builds back to back. `scripts/soak.sh` plays seven tracks in the editor for 10 minutes, judging the rate memory grows at, then reopens the editor 200 times; `scripts/soak.sh 60` runs the hour-long version. Build the benchmarks with the `bench` preset (optimised) or `bench-debug` (as the development plugin).

## Releasing

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/soak.sh` (about 12 minutes): memory must not grow faster than 0.5 MB a minute, frames must stay steady, and reopening the editor must leave nothing behind. After changing something long-lived (capture buffers, the musical clock, the page's memory use), also run the hour-long `scripts/soak.sh 60`.
4. Run `scripts/package.sh`. It builds the Release bundle, verifies both architectures and the macOS 13.1 minimum, and writes `build/release/Sineless-<version>-macOS.zip`. On Windows, run `scripts\package.ps1`: it builds the x64 and ARM64 Release binaries into one bundle, checks each binary's architecture and that it needs no Visual C++ runtime DLLs, and writes `build\release\Sineless-<version>-Windows.zip`.
5. Install the zipped bundle and test it in each host you can, following the protocol in [HOSTS.md](HOSTS.md), at least in Ableton Live, on macOS and on Windows. Update the matrix there.
6. Tag the commit `vx.y.z` and attach both zips to a GitHub release.
