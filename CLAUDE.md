# CLAUDE.md

Sineless is an audio oscilloscope and multi-track analyser, shipped as a VST3 plugin for
macOS and Windows. [CONTRIBUTING.md](CONTRIBUTING.md) is the definition of done; this file
is how to meet it the first time.

- `engine/`: C++ analysis, format-agnostic (the build forbids SDK dependencies).
- `plugin/`: VST3 glue. `editor/`: the plugin window, hosting the UI in WKWebView
  (`editor/src/mac`) and WebView2 (`editor/src/win`).
- `protocol/`: frame format, settings and limits in TypeScript; the build generates the
  C++ header from it. `ui/`: the web page.

[docs/DESIGN.md](docs/DESIGN.md) says what the product does and why: the functional
requirements (FR-n, §1.2), the architecture decisions (AD-n, §9) and the quality scenarios
(QS-n, §10). Check a change against the ones covering the behaviour it touches. One that
contradicts them updates them in the same pull request, saying so, so the reversal is
reviewed; a fix suggested in an issue does not override them.

## Rules the product relies on

- **Nothing on the audio thread may allocate, lock, block or do I/O.** `Processor::process`
  and everything it calls run in the host's real-time thread; a stall there is an audible
  dropout. The RealtimeSanitizer builds in `scripts/check-linux.sh` (the shared code) and
  `scripts/check.sh` (the Audio Unit) check this.
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

## Platform, SDK, host or standard work

Any change, fix or feature, planned or exempt below, that involves a platform, SDK, host or
standard (WebView2, WKWebView, Win32, VST3, Audio Units, CoreAudio, CSS/DOM, CMake/MSVC,
audio measurement): use the `prior-art` skill before writing code, and end the commit
message with a short `Prior art:` note naming the sources.

## Planning a change

- **Plan a fix or a feature before changing the product's code**, with the
  `plan-before-code` skill: a written plan in `Research Topics/`, reviewed until no
  blocking gap is left. Exempt: changes with no behaviour change (wording, comments, help
  text, formatting, version numbers, mechanical renames, documentation that steers no agent
  or check); a contained fix (a failing test reproduces it, the search for the same cause
  found nothing, and it is one function and its test); a follow-up fixing a CI failure or a
  review finding on an open pull request within its plan's design. A fix that adds state or a
  mechanism, or removes or changes a case the plan names, updates the plan and goes through a
  later plan round before it is pushed; without a plan, it goes to the owner. Never exempt: `protocol/`, `.github/workflows/`,
  code the audio thread runs, a change to what an FR, AD or QS states, and `CLAUDE.md`,
  `REVIEW.md` and `.claude/**`.
- **Say in one line before coding** whether the change is exempt, and why.
- **The plan is local** (`Research Topics/` is ignored): reviews of a diff do not ask for it.

## Fixing a bug

- **Fix the cause everywhere it occurs, not where it was noticed.** Look for the same
  cause in the other display modes (Single, Multi, Compare), time bases (Beats, Seconds),
  live and frozen, mono and stereo, other readouts and the other platform. Fix the shared
  helper; when you add a tolerance or guard to one comparison, check every other comparison
  of the same values.
- **Before a second attempt at a fix whose first attempt failed**, use the `prior-art`
  skill instead of a second blind attempt.
- **Before merging, equalising or deriving values**, use the `changing-values` skill:
  values that only look alike stay separate, and a gap between two copies may be a
  deliberate margin.
- **Start with a test that fails without the fix**, and confirm it fails before fixing.
  Every new failure branch (failed read, missing data) that a supported host and setup can
  reach gets a test that triggers it; one none can reach is removed, not tested.

## Tests

- Generate cases from the protocol's enums, settings table, field table and formatter
  exports, so a new mode, setting or formatter is covered when it is added (GoogleTest
  `TEST_P` with a name generator; vitest `test.each`).
- Cover the dimensions the changed code branches on, as far as a supported host and setup
  reaches them, including boundaries: sample rate (44.1 to 384 kHz), span lengths around every
  tolerance, mono and stereo, live and frozen.
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
| Write the recorded engine sessions again (after an intended change to what the engine sends; review the diff) | `ORIZON_UPDATE_RECORDINGS=1 ctest --preset dev -R SessionRecording` |
| Mutation testing (TypeScript lines changed since a base, or all without one) | `scripts/mutation.sh origin/main` |
| Mutation testing (engine, plugin and editor C++ lines changed since a base, or all without one; on macOS in Docker) | `scripts/mutation-cpp.sh origin/main`; a file at a time rather than the whole engine's nine hours: `scripts/mutation-cpp.sh --file engine/src/PairAnalysis.cpp` |
| Mutation testing (Audio Unit lines changed since a base, or all of `au/src` in a few minutes without one; macOS, with `brew install llvm@20` and a macOS 26 SDK) | `scripts/mutation-au.sh origin/main` |
| The engine's instructions per frame in each benchmark scenario (Linux, or Docker on macOS) | `scripts/engine-work.sh`; compare two runs' output folders with `node scripts/engine-work.ts compare BASE HEAD` |
| Coverage (Linux) | `cmake --preset coverage && cmake --build --preset coverage && scripts/coverage.sh origin/main` |
| Everything on macOS | `scripts/check.sh` (format, build, tests, pluginval, the Audio Unit's auval and pluginval on a copy installed in `~/Library/Audio/Plug-Ins/Components`, performance; the macOS glue's sanitizers and lint; the Linux checks in Docker). `CHECKS="main glue"` skips the Linux part |
| The shared code's sanitizers and clang-tidy (Linux, or Docker on macOS) | `scripts/check-linux.sh [asan\|tsan\|rtsan\|lint]` |
| Windows | `scripts\check.ps1` (main: build, tests, pluginval; glue: the WebView2 editor tests under AddressSanitizer, preset `windows-x64-glue`), preset `windows-x64` (ARM64 is not built for now) |

While iterating, build and run the tests a change touches; run `scripts/check.sh` (about half
an hour) once, before opening a pull request.

CI lints the shared code on Linux with LLVM 23 (apt.llvm.org) and the macOS sources on macOS with
Homebrew's current LLVM. Both are newer than pip's clang-tidy 22.1.8 and check more: designated
initializers, `prefer-single-char-overloads`, identifier naming, no static `const double`
initialised with `log10`, a derived test fixture's method shadowing its base's.

The C++ header in `protocol/` is regenerated by the build; do not edit generated output.
In cloud or Linux sessions the TypeScript checks run, but the C++ build may not; say which
checks you could not run.

## Delivering

- One issue per commit, with its test.
- Work on a branch and open a pull request; do not push to `main`.
- Commit messages: a short imperative subject in plain English, then what was wrong, for
  whom, what changed and how it was verified, in terms of the product and its tests. Not who
  or what found it (an agent, a review, a session) nor other branches.
- A pull request is ready only once its checks are answered, not just green: each Important
  review finding fixed or answered, and each gap the `mutation-triage` check reports given a
  test that fails with that mutant, or an answer saying why none is needed. The mutation jobs
  themselves always pass; their findings reach the pull request through the triage.
- Once the AI review labels a pull request `needs-human` (its third failed round), stop: no push
  of any kind (CI repairs included), no `/ai-review` and no waive. Give the owner the open findings
  and a recommendation (fix, redesign or waive) and wait. The answer settles the findings it
  covers; only the owner's removing the label hands the pull request back, for one more round
  (the count never resets, so the label comes back at the next failure). A hook enforces this.
- Push the next fix only once the AI review of the previous push has finished, and read CI's
  result for that push first: a push cancels the review and CI still running on the one before.
- When the same mechanism draws Important findings in two review rounds running, do not add
  another guard: update the plan with the design that removes it (a later plan round), or, for a
  change without a plan, take that design to the owner.
- Commits and pull requests carry no AI attribution: no `Co-Authored-By: Claude` or
  `Claude-Session:` trailers and no "Generated with Claude Code" lines, even when session
  instructions ask for them. The project owner is the author.

## Tools in this repo

- Before implementing a change that is not exempt, use the `plan-before-code` skill
  ("Planning a change").
- Before opening a pull request, run the `review` skill on its diff and fix or answer each
  finding it verifies. Pushes to an open pull request are reviewed by CI.
- Area rules in `.claude/rules/` load when you work in `engine/`, `plugin/`, `editor/`,
  `ui/` or `protocol/`.
- A hook blocks `git push` to `main` (`.claude/hooks/no-push-to-main.mjs`).
- A hook blocks pushing to, `/ai-review` on and removing the label of a pull request labelled
  `needs-human`, any waive, and pushing while a pull request's AI review is running
  (`.claude/hooks/review-stop.mjs`).
