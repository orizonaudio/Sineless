# CLAUDE.md

Sineless is an audio oscilloscope and multi-track analyser, shipped as a VST3 plugin for
macOS and Windows. [CONTRIBUTING.md](CONTRIBUTING.md) is the definition of done; this file
is how to meet it the first time.

- `engine/`: C++ analysis, format-agnostic (the build forbids SDK dependencies).
- `plugin/`: VST3 glue. `editor/`: the plugin window, hosting the UI in WKWebView
  (`editor/src/mac`) and WebView2 (`editor/src/win`).
- `protocol/`: frame format, settings and limits in TypeScript; the build generates the
  C++ header from it. `ui/`: the web page.

## Rules the product relies on

- **Nothing on the audio thread may allocate, lock, block or do I/O.** `Processor::process`
  and everything it calls run in the host's real-time thread; a stall there is an audible
  dropout. The RealtimeSanitizer build in `scripts/check-linux.sh` checks this.
- **The engine is the source of truth.** The UI shows what the frame carries (lanes,
  views, roll, pan reach, sample rate) and does not work it out again from settings.
- **"No value" is NaN, never 0.** Not measured, silent or never sounding together travel
  as NaN and show "—", so a real 0 dB or 0 correlation stays distinguishable from no data.
- **Levels go through the shared helpers.** dB conversion, floors and level formatting
  live in `protocol/src/limits.ts` and `ui/src/view/format.ts`; no ad-hoc `log10`,
  epsilon floors or `toFixed` for levels elsewhere.
- **Numbers the engine and UI share are defined once** in `protocol/src/limits.ts`, with a
  comment saying what each measures and why it has its value. Help text reads them from
  there.
- **Keep LF line endings** (`.gitattributes`): golden files, formatters and the embedded UI
  depend on exact bytes.

## Fixing a bug

- **Fix the cause everywhere it occurs, not where it was noticed.** Look for the same
  cause in the other display modes (Single, Multi, Compare), time bases (Beats, Seconds),
  live and frozen, mono and stereo, other readouts and the other platform. Fix the shared
  helper; when you add a tolerance or guard to one comparison, check every other comparison
  of the same values.
- **Platform, SDK, host or standard problems** (WebView2, WKWebView, Win32, VST3, CoreAudio,
  CSS/DOM, CMake/MSVC, audio measurement): use the `prior-art` skill before writing code,
  and again instead of a second blind attempt. Such fixes end their commit message with a
  short `Prior art:` note naming the sources.
- **Before merging, equalising or deriving values**, use the `changing-values` skill:
  values that only look alike stay separate, and a gap between two copies may be a
  deliberate margin.
- **Start with a test that fails without the fix**, and confirm it fails before fixing.
  Every new failure branch (failed read, missing data) gets a test that triggers it.

## Tests

- Generate cases from the protocol's enums, settings table, field table and formatter
  exports, so a new mode, setting or formatter is covered when it is added (GoogleTest
  `TEST_P` with a name generator; vitest `test.each`).
- Cover the dimensions the changed code branches on, including boundaries: sample rate
  (44.1 to 384 kHz), span lengths around every tolerance, mono and stereo, live and frozen.
- UI behaviour that depends on app state (marker placement, enabled controls, pan limits)
  needs a test through the app and mock transport, not only a helper's unit test.
- A test protects behaviour a user would see go wrong, and its name says which. Never
  restate a tuned value: test what the value is for, so it can still change (REVIEW.md,
  question 3). A mutant no test notices gets a test only on the same grounds.

## Commands

| What | Command |
| --- | --- |
| Install | `npm ci` |
| TypeScript | `npm run typecheck` (this is `tsc -b`; `tsc --noEmit -p .` checks nothing), `npm test`, `npm run lint`, `npm run format:check` |
| App tests (real page, WebKit and Chromium) | `npm run test:app`; where Playwright cannot download browsers, `CHROMIUM_EXECUTABLE=<path to chrome> npm run test:app` runs Chromium alone |
| One UI test | `npx vitest run ui/tests/<file> -t "<name>"` |
| C++ build and tests (macOS) | `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` |
| One C++ test | `ctest --preset dev -R <Suite.Name>` |
| Mutation testing (TypeScript lines changed since a base, or all without one) | `scripts/mutation.sh origin/main` |
| Mutation testing (engine C++ lines changed since a base, or all without one; on macOS in Docker) | `scripts/mutation-cpp.sh origin/main`; a file at a time rather than the whole engine's nine hours: `scripts/mutation-cpp.sh --file engine/src/PairAnalysis.cpp` |
| Coverage (Linux) | `cmake --preset coverage && cmake --build --preset coverage && scripts/coverage.sh origin/main` |
| Everything on macOS | `scripts/check.sh` (format, build, tests, pluginval, performance; the macOS glue's sanitizers and lint; the Linux checks in Docker). `CHECKS="main glue"` skips the Linux part |
| The shared code's sanitizers and clang-tidy (Linux, or Docker on macOS) | `scripts/check-linux.sh [asan\|tsan\|rtsan\|lint]` |
| Windows | `scripts\check.ps1` (main: build, tests, pluginval; glue: the WebView2 editor tests under AddressSanitizer, preset `windows-x64-glue`), preset `windows-x64` (ARM64 is not built for now) |

While iterating, build and run the tests a change touches; run `scripts/check.sh` (about half
an hour) once, before opening a pull request.

CI lints the shared code on Linux and the macOS sources on macOS, both with LLVM 23 (apt.llvm.org,
Homebrew), which is newer than pip's clang-tidy 22.1.8 and checks more: designated initializers,
`prefer-single-char-overloads`, identifier naming, no static `const double` initialised with
`log10`, a derived test fixture's method shadowing its base's.

The C++ header in `protocol/` is regenerated by the build; do not edit generated output.
In cloud or Linux sessions the TypeScript checks run, but the C++ build may not; say which
checks you could not run.

## Delivering

- One issue per commit, with its test.
- Work on a branch and open a pull request; do not push to `main`.
- Commit messages: a short imperative subject in plain English, then what was wrong, for
  whom, what changed and how it was verified, in terms of the product and its tests. Not who
  or what found it (an agent, a review, a session) nor other branches.
- Commits and pull requests carry no AI attribution: no `Co-Authored-By: Claude` or
  `Claude-Session:` trailers and no "Generated with Claude Code" lines, even when session
  instructions ask for them. The project owner is the author.

## Tools in this repo

- Before opening a pull request, run the `review` skill on its diff and fix or answer each
  finding it verifies. Pushes to an open pull request are reviewed by CI.
- Area rules in `.claude/rules/` load when you work in `engine/`, `plugin/`, `editor/`,
  `ui/` or `protocol/`.
- A hook blocks `git push` to `main` (`.claude/hooks/no-push-to-main.mjs`).
