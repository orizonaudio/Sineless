# Contributing

## Picking up an issue

Assign yourself to an issue before you start on it, and leave issues someone else is assigned to alone. Unassign yourself if you stop.

## Licence of contributions

You contribute under the project's licence: GPL-3.0-only, with the additional permission stated in the README's Licence section, which lets Sineless be conveyed together with the logo files in `ui/src/brand/`. Opening a pull request means you agree to that.

## Definition of Done

Every change:

- [ ] `scripts/check.sh` passes locally. CI runs its parts as jobs of their own (TypeScript, the macOS main and glue jobs, the Linux checks), and `scripts\check.ps1`'s on Windows x64 (main and glue), on every pull request that changes more than documentation (Markdown, or files under `docs/`; see `scripts/changed-code.ts`), and the ruleset on `main` requires them all except the performance and fuzz jobs, and the engine-work jobs until their counts have matched exactly for a week. A change to Windows code, or to anything built on Windows, also passes `scripts\check.ps1` on Windows.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Any new dependency was agreed on first and is pinned.
- [ ] A number the engine and the UI both use (a limit, a threshold, an interval, a layout size) is defined once in `protocol/src/limits.ts`, which the C++ header is generated from, and help text reads it from there too.

## Performance

`scripts/performance.sh` (macOS) and `scripts\performance.ps1` (Windows) enforce performance limits, measured on optimised builds of two benchmarks. The check scripts run them unless `SKIP_PERFORMANCE=1`; CI runs them as a separate job that never fails: shared CI machines are too slow and noisy for the speed limits (frames/s, CPU, the audio thread's share), so it measures them and shows them in the job summary (`PERF_SPEED=report`), and warns only when a leak limit (what reopening the editor leaves behind) is missed or a benchmark does not run to the end.

- `process_bench`: real processor instances driven through the VST3 API at every common block size and sample rate. Eight instances together must stay within 5% of each block's deadline at p99.9. `scripts\performance.ps1` runs it on GitHub's Windows machines, or anywhere with `RUN_BENCH=1`.
- `perf_bench`: the real editor page in a WKWebView, with tracks playing in real time. It plays each scenario listed in `scripts/performance-limits.json` (`--scenario`, from `engine/bench/include/bench/Scenarios.h`): seven tracks over four bars in Multi and in Compare, fifteen over sixteen bars at 60 BPM and 96 kHz in Multi, and the worst case, Compare with all fifteen checked and Preview fix on. The page records when the display ticked and when each new frame was drawn (`ui/src/frame-times.ts`), so a run reports the time between new frames at the median, 95th and 99th percentiles, the share that came late (more than one and a half frames after the one before), and the CPU each part spent per frame drawn, which frames/s alone hides once it reaches the page's 60. Each run writes its measurements to `build/performance/<scenario>.json` in Bencher Metric Format.
- The editor's limits are set per machine (`PERF_MACHINE`, by default `developer-mac`) and display scale, as a frame at twice the pixel density draws twice the columns: on a machine or scale with none, the scenarios are measured and reported, not judged. A reference machine is a new entry in the file. Opening and closing the editor 50 times must leave nothing behind on any machine.

To compare changes, `scripts/bench.sh` prints CPU by part, frames/s, memory and data per frame for a fixed set of scenarios, at 48 kHz and 120 BPM unless a scenario says otherwise (`perf_bench` takes `--rate` and `--tempo`). CPU in % of a core varies with system load, so compare builds back to back. `scripts/soak.sh` plays seven tracks in the editor for 10 minutes, judging the rate memory grows at, then reopens the editor 200 times; `scripts/soak.sh 60` runs the hour-long version. Build the benchmarks with the `bench` preset (optimised) or `bench-debug` (as the development plugin).

### Engine work

On shared machines, time per frame cannot tell a slower change from a busier machine, but the instructions a frame takes can. `analysis_bench` (`engine/bench/AnalysisBench.cpp`) runs the editor's analysis with no editor, page or audio thread, on audio played in steps, so that two runs of one build do exactly the same work, for each scenario in `engine/bench/include/bench/Scenarios.h`: the worst case (Compare, 15 tracks all checked, Preview fix, 16 bars at 60 BPM and 96 kHz), Multi over the same sweep and zoomed into it, Compare rolling and frozen while panning, and Single over a sweep, triggered and on a mono track. `scripts/engine-work.sh` counts each scenario's instructions per measured frame under Valgrind's Cachegrind (on macOS, in Docker), with the worst case's last-level cache misses beside them, for the memory its sums read, which instructions do not show.

On every pull request, CI counts the base and the head, and the head again, each in a job of its own (`engine-work-base`, `engine-work-head`, `engine-work-again`), and the `engine-work` job compares them (`scripts/engine-work.ts`). It fails when a gated scenario's instructions per frame rise by more than 1%, or when the head counts differently the second time, which means the measurement, not the change, moved. When a rise is intended, a maintainer adds the `performance-accepted` label with a comment saying why, and runs the `engine-work` job again. Results are in Bencher Metric Format (`build/engine-work/results.json`), so they can be kept and charted later.

Run on its own, `analysis_bench --scenario=compare-15-worst` times each frame instead (median and 99th percentile); `--list` names the scenarios and `--columns` sets the screen width in device pixels (2560 by default, a 1280-pixel screen at twice its density).

## Test levels

What each level covers, where it lives and where it runs. Planned levels are not written yet;
CI marks where each will run.

| Level | What it checks | Where | Runs |
| --- | --- | --- | --- |
| Static | Types, lint, format | `tsc`, ESLint, Prettier, clang-format, clang-tidy | Every PR |
| Unit | One module at a time | `engine/tests`, `protocol/tests`, `ui/tests` | Every PR |
| Component | The plugin through the VST3 API; the editor's session, frame flow and sizing | `plugin/tests`, `editor/tests` | Every PR |
| Integration | The editor with its page in a real WKWebView or WebView2 | `editor/tests/EditorTests.mm`, `editor/tests/EditorTestsWin.cpp` | Every PR |
| App | The real page against the mock back end, in WebKit and Chromium: controls, readouts, lanes, pan limits | `ui/tests/app`, `npm run test:app` | Every PR |
| Contract | C++ and TypeScript agree on frames, messages and saved state | `protocol/tests/data`, `engine/tests/data` | Every PR |
| Conformance | The VST3 and Audio Unit rules hosts rely on | VST3 validator, pluginval at strictness 10; for the Audio Unit, Apple's auval (strict, in and out of process) and pluginval on a copy installed for the user | Every PR |
| Sanitizers | Real-time safety, data races, memory errors, leaks and undefined behaviour | The shared code on Linux (`scripts/check-linux.sh`): RealtimeSanitizer, ThreadSanitizer, AddressSanitizer with LeakSanitizer and UndefinedBehaviorSanitizer; the macOS editor's web view glue: AddressSanitizer with UndefinedBehaviorSanitizer, ThreadSanitizer; the Audio Unit: RealtimeSanitizer, AddressSanitizer with UndefinedBehaviorSanitizer, and for its view ThreadSanitizer; the Windows editor's: AddressSanitizer (MSVC has neither of the others) | Every PR |
| Fault injection | Failed and short reads, a page that never loads | `engine/tests`, `plugin/tests`, `editor/tests` | Every PR |
| Performance | Audio-thread deadline, frame rate, CPU, memory | `scripts/performance.sh`, `scripts/performance.ps1` | Every PR, reported |
| Engine work | Instructions per frame of the analysis in each benchmark scenario, head against base, under Cachegrind | `engine/bench`, `scripts/engine-work.sh`, `scripts/engine-work.ts` | Every PR: fails on a rise over 1% in a gated scenario |
| Soak | Memory growth and reopening over time | `scripts/soak.sh` | Before a release |
| End-to-end | Sineless in REAPER, scripted | `scripts/windows` | With the full protocol (Releasing) |
| Acceptance | The smoke test in Live on macOS, and in Logic or a Windows host when their code changes; occasionally the full protocol | [HOSTS.md](HOSTS.md) | Before a release (the full protocol when due, Releasing) |
| Matrix | Every sample rate from 44.1 to 384 kHz, spans around each tolerance, mono and stereo, live and frozen | `engine/tests/MatrixTests.cpp` | Every PR |
| Work per frame | A frame's cost follows what changed, not the size of what is shown: samples reduced, summed and read and matches searched, counted (`AnalysisWork`), not timed | `engine/tests/MatrixTests.cpp` ("Work per frame") | Every PR |
| Signal corpus | Approved frames for tones, DC, silence, NaN, inverted and delayed pairs | `engine/tests/corpus`, from `engine/tests/SignalCorpusTests.cpp` | Every PR |
| Fuzzing | State loading, setting values, the page's messages and frame decoding against malformed input | `engine/tests/fuzz` (libFuzzer, Linux: `cmake --preset fuzz && cmake --build --preset fuzz && scripts/fuzz.sh [seconds]`), `*.properties.test.ts` (fast-check) | Every PR, five minutes, and nightly, half an hour, from a corpus kept on main; their seeds and fixed crashes' inputs (`engine/tests/fuzz/regressions/<fuzzer>/`) replay in every platform's tests (`ctest -R '^fuzz\.'`) |
| Coverage | Lines the tests run, reported on changed lines | `scripts/coverage.sh` (llvm-cov, Vitest V8) | Every PR, reported |

The signal corpus plays each signal of the table in `engine/tests/SignalCorpusTests.cpp` through the analyzer and compares the last frame's readouts, views or lanes, matches and summing gains with its approved JSON file, numbers within a tolerance that allows for rounding differences between compilers and platforms, and NaN as `"NaN"`. Each case also states what its signal must read (a 0.5 sine peaks at −6.02 dBFS, an inverted copy correlates at −1), so a wrong value cannot be approved. To add a signal, add a line to the table. When a change to what the engine reports is intended, write the files again and review the diff before committing it:

```sh
ORIZON_UPDATE_CORPUS=1 ctest --preset linux -R SignalCorpus   # or --preset dev on macOS
```

On Windows, set `ORIZON_UPDATE_CORPUS=1` in the environment before running `ctest`. The files are written with LF line endings and as Prettier formats them.

## Releasing

Open a **Release** issue from its template first: it lists every step below and the smoke test, to be ticked, or answered in a comment, before the draft is published.

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`, and as the placeholder of the version field in the bug report and host compatibility issue templates.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/soak.sh` (about 12 minutes): memory must not grow faster than 0.5 MB a minute, frames must stay steady, and reopening the editor must leave nothing behind. After changing something long-lived (capture buffers, the musical clock, the page's memory use), also run the hour-long `scripts/soak.sh 60`.
4. Once the version change has merged, tag that commit of `main` `vx.y.z` and push the tag (`git tag vx.y.z <commit>`, then `git push origin vx.y.z`). The Release workflow (`.github/workflows/release.yml`) checks that the tagged commit is on `main` and that the tag matches the version in `CMakeLists.txt`, then builds both zips and both installers on GitHub's runners with `scripts/package.sh` (the VST3 plugin and the Audio Unit, each with both architectures and a macOS 13.1 minimum; the installer installs both into the user's home folder without a password, either of them left out under Customize, which the runner checks by installing it) and `scripts\package.ps1` (the x64 binary, of the right machine type and not needing the Visual C++ runtime DLLs; ARM64 is not built for now; the installer, built with Inno Setup from `scripts\installer.iss`, which the runner installs, checks and uninstalls). It attests where they were built and creates a draft release with them, their SHA-256 checksums and notes from the merged pull requests. To try it before tagging, run the workflow by hand from the Actions tab: it builds and checks every download and keeps them with the run, but creates no release.
5. Install the draft's downloads and run the smoke test in [HOSTS.md](HOSTS.md):
   - in Ableton Live on macOS, for every release;
   - in Logic Pro or GarageBand too when the release changes anything that builds into the Audio Unit or its package (`au/`, the macOS editor in `editor/src/mac/`, `cmake/AudioUnitVersion.cmake`, `cmake/Branding.cmake`, `scripts/package.sh`, `scripts/validate-au.sh`);
   - in a Windows host too when it changes any Windows-only code or build setting (`editor/src/win/`, `_WIN32` branches, Windows conditions in the CMake files, `scripts/package.ps1`, `scripts/installer.iss`, `scripts/windows-env.ps1`).

   Run the full protocol there instead, in each host you can, before a release that changes what Sineless takes from hosts (the transport, tempo, track details, state, the editor's sizing) or its memory (the sweeps, capture buffers), after a tested host's major update, and at least once a quarter in Live on macOS and on Windows; on Windows, run the scripted REAPER test (`scripts/windows`) with it. Record the hosts and the test in the matrix in HOSTS.md.
6. Publish the draft release.

The packaging scripts also run locally, writing `build/release/Sineless-<version>-macOS.zip`, `build/release/Sineless-<version>-macOS.pkg`, `build\release\Sineless-<version>-Windows.zip` and `build\release\Sineless-<version>-Windows-Setup.exe` (with Inno Setup installed: `winget install JRSoftware.InnoSetup`).
