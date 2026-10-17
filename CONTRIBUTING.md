# Contributing

## Picking up an issue

Assign yourself to an issue before you start on it, and leave issues someone else is assigned to alone. Unassign yourself if you stop.

## Definition of Done

Every change:

- [ ] `scripts/check.sh` passes locally. CI runs its parts as jobs of their own (TypeScript, the macOS main and glue jobs, the Linux checks), and `scripts\check.ps1`'s on Windows x64 (main and glue), on every pull request that changes more than documentation (Markdown; see `scripts/changed-code.ts`), and the ruleset on `main` requires them all except the performance and fuzz jobs. A change to Windows code, or to anything built on Windows, also passes `scripts\check.ps1` on Windows.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Any new dependency was agreed on first and is pinned.
- [ ] A number the engine and the UI both use (a limit, a threshold, an interval, a layout size) is defined once in `protocol/src/limits.ts`, which the C++ header is generated from, and help text reads it from there too.

## Performance

`scripts/performance.sh` (macOS) and `scripts\performance.ps1` (Windows) enforce performance limits, measured on optimised builds of two benchmarks. The check scripts run them unless `SKIP_PERFORMANCE=1`; CI runs them as a separate job that never fails: shared CI machines are too slow and noisy for the speed limits (frames/s, CPU, the audio thread's share), so it measures them and shows them in the job summary (`PERF_SPEED=report`), and warns only when a leak limit (what reopening the editor leaves behind) is missed or a benchmark does not run to the end.

- `process_bench`: real processor instances driven through the VST3 API at every common block size and sample rate. Eight instances together must stay within 5% of each block's deadline at p99.9. `scripts\performance.ps1` runs it on GitHub's Windows machines, or anywhere with `RUN_BENCH=1`.
- `perf_bench`: the real editor page in a WKWebView, with tracks playing in real time. Seven tracks over four bars in Multi and in Compare, and fifteen tracks over sixteen bars at 60 BPM and 96 kHz (a 64 s sweep), must draw at 45 frames/s or more with the analysis thread under its budget, and opening and closing the editor 50 times must leave nothing behind.

To compare changes, `scripts/bench.sh` prints CPU by part, frames/s, memory and data per frame for a fixed set of scenarios, at 48 kHz and 120 BPM unless a scenario says otherwise (`perf_bench` takes `--rate` and `--tempo`). CPU in % of a core varies with system load, so compare builds back to back. `scripts/soak.sh` plays seven tracks in the editor for 10 minutes, judging the rate memory grows at, then reopens the editor 200 times; `scripts/soak.sh 60` runs the hour-long version. Build the benchmarks with the `bench` preset (optimised) or `bench-debug` (as the development plugin).

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
| Conformance | The VST3 rules hosts rely on | VST3 validator, pluginval at strictness 10 | Every PR |
| Sanitizers | Real-time safety, data races, memory errors, leaks and undefined behaviour | The shared code on Linux (`scripts/check-linux.sh`): RealtimeSanitizer, ThreadSanitizer, AddressSanitizer with LeakSanitizer and UndefinedBehaviorSanitizer; the macOS editor's web view glue: AddressSanitizer with UndefinedBehaviorSanitizer, ThreadSanitizer; the Windows editor's: AddressSanitizer (MSVC has neither of the others) | Every PR |
| Fault injection | Failed and short reads, a page that never loads | `engine/tests`, `plugin/tests`, `editor/tests` | Every PR |
| Performance | Audio-thread deadline, frame rate, CPU, memory | `scripts/performance.sh`, `scripts/performance.ps1` | Every PR, reported |
| Soak | Memory growth and reopening over time | `scripts/soak.sh` | Before a release |
| End-to-end | Sineless in REAPER, scripted | `scripts/windows` | Before a release |
| Acceptance | The protocol in each host | [HOSTS.md](HOSTS.md) | Before a release |
| Matrix | Every sample rate from 44.1 to 384 kHz, spans around each tolerance, mono and stereo, live and frozen | `engine/tests/MatrixTests.cpp` | Every PR |
| Signal corpus | Approved frames for tones, DC, silence, NaN, inverted and delayed pairs | `engine/tests/corpus`, from `engine/tests/SignalCorpusTests.cpp` | Every PR |
| Fuzzing | State loading, setting values, the page's messages and frame decoding against malformed input | `engine/tests/fuzz` (libFuzzer, Linux: `cmake --preset fuzz && cmake --build --preset fuzz && scripts/fuzz.sh [seconds]`), `*.properties.test.ts` (fast-check) | Every PR, five minutes, and nightly, half an hour, from a corpus kept on main; their seeds and fixed crashes' inputs (`engine/tests/fuzz/regressions/<fuzzer>/`) replay in every platform's tests (`ctest -R '^fuzz\.'`) |
| Coverage | Lines the tests run, reported on changed lines | `scripts/coverage.sh` (llvm-cov, Vitest V8) | Every PR, reported |

The signal corpus plays each signal of the table in `engine/tests/SignalCorpusTests.cpp` through the analyzer and compares the last frame's readouts, views or lanes, matches and summing gains with its approved JSON file, numbers within a tolerance that allows for rounding differences between compilers and platforms, and NaN as `"NaN"`. Each case also states what its signal must read (a 0.5 sine peaks at −6.02 dBFS, an inverted copy correlates at −1), so a wrong value cannot be approved. To add a signal, add a line to the table. When a change to what the engine reports is intended, write the files again and review the diff before committing it:

```sh
ORIZON_UPDATE_CORPUS=1 ctest --preset linux -R SignalCorpus   # or --preset dev on macOS
```

On Windows, set `ORIZON_UPDATE_CORPUS=1` in the environment before running `ctest`. The files are written with LF line endings and as Prettier formats them.

## Releasing

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/soak.sh` (about 12 minutes): memory must not grow faster than 0.5 MB a minute, frames must stay steady, and reopening the editor must leave nothing behind. After changing something long-lived (capture buffers, the musical clock, the page's memory use), also run the hour-long `scripts/soak.sh 60`.
4. Once the version change has merged, tag that commit of `main` `vx.y.z` and push the tag (`git tag vx.y.z <commit>`, then `git push origin vx.y.z`). The Release workflow (`.github/workflows/release.yml`) checks that the tagged commit is on `main` and that the tag matches the version in `CMakeLists.txt`, then builds both zips and the macOS installer on GitHub's runners with `scripts/package.sh` (both architectures, macOS 13.1 minimum; the installer installs into the user's home folder without a password, which the runner checks by installing it) and `scripts\package.ps1` (the x64 binary, of the right machine type and not needing the Visual C++ runtime DLLs; ARM64 is not built for now). It attests where they were built and creates a draft release with them, their SHA-256 checksums and notes from the merged pull requests. To try it before tagging, run the workflow by hand from the Actions tab: it builds and checks every download and keeps them with the run, but creates no release.
5. Install the draft's downloads (the macOS installer and the Windows zip at least) and test them in each host you can, following the protocol in [HOSTS.md](HOSTS.md), at least in Ableton Live, on macOS and on Windows. Update the matrix there.
6. Publish the draft release.

The packaging scripts also run locally, writing `build/release/Sineless-<version>-macOS.zip`, `build/release/Sineless-<version>-macOS.pkg` and `build\release\Sineless-<version>-Windows.zip`.
