# Contributing

## Definition of Done

Every change:

- [ ] `scripts/check.sh` passes locally. CI runs the same script, on demand.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Any new dependency was agreed on first and is pinned.

## Performance

`scripts/check.sh` enforces performance limits, measured on optimised builds of two benchmarks:

- `process_bench`: real processor instances driven through the VST3 API at every common block size and sample rate. Eight instances together must stay within 5% of each block's deadline at p99.9.
- `perf_bench`: the real editor page in a WKWebView, with tracks playing in real time. Seven tracks must draw at 45 frames/s or more with the analysis thread under its budget, and opening and closing the editor 50 times must leave nothing behind.

To compare changes, `scripts/bench.sh` prints CPU by part, frames/s and data per frame for a fixed set of scenarios. CPU in % of a core varies with system load, so compare builds back to back. `scripts/soak.sh` plays seven tracks in the editor for 10 minutes, judging the rate memory grows at, then reopens the editor 200 times; `scripts/soak.sh 60` runs the hour-long version. Build the benchmarks with the `bench` preset (optimised) or `bench-debug` (as the development plugin).

## Releasing

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/soak.sh` (about 12 minutes): memory must not grow faster than 0.5 MB a minute, frames must stay steady, and reopening the editor must leave nothing behind. After changing something long-lived (capture buffers, the musical clock, the page's memory use), also run the hour-long `scripts/soak.sh 60`.
4. Run `scripts/package.sh`. It builds the Release bundle, verifies both architectures and the macOS 13 minimum, and writes `build/release/O-Scope-<version>-macOS.zip`.
5. Install the zipped bundle and test it in each host you can, following the protocol in [HOSTS.md](HOSTS.md), at least in Ableton Live. Update the matrix there.
6. Tag the commit `vx.y.z` and attach the zip to a GitHub release.
