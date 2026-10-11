# CLAUDE.md

O-Scope is an audio oscilloscope and multi-track analyser, shipped as a VST3 plugin for
macOS and Windows.

- `engine/`: C++ analysis, format-agnostic; the build forbids SDK dependencies.
- `plugin/`: VST3 glue.
- `editor/`: the plugin window, hosting the UI in WKWebView (`editor/src/mac`) and
  WebView2 (`editor/src/win`).
- `protocol/`: the shared frame format, settings and limits, in TypeScript; the C++
  header is generated from it.
- `ui/`: the web page.

[CONTRIBUTING.md](CONTRIBUTING.md) is the definition of done. This file covers how to
work so that the definition of done is met the first time.

## 1. Research before you write a fix

When a problem involves a platform, SDK, host or standard, research it **before writing
code**. That includes WebView2, WKWebView, Win32 DPI and input, VST3, Audio Units,
CoreAudio, DOM events, CSS, CMake/MSVC, and audio measurement. Find and read:

1. **The vendor's guidance:** API reference, best-practice and known-issue pages,
   official samples, and the vendor's issue tracker (e.g. MicrosoftEdge/WebView2Feedback).
2. **How 2–3 widely used projects solve it:** JUCE, iPlug2, VSTGUI, CHOC, DPF,
   wry/Tauri, Chromium, the VST3 SDK. Read their source, not forum posts, and cite the
   file and line.
3. **The relevant standard**, where one exists: AES17, ITU-R BS.1770, EBU R128, CSS specs.

If a documentation site is blocked, read its source repository instead:

- MicrosoftDocs/edge-developer
- MicrosoftDocs/win32 and MicrosoftDocs/sdk-api
- steinbergmedia/vst3_pluginterfaces, vst3_public_sdk and vst3_dev_portal
- apple/AudioUnitSDK
- the XML docs inside the WebView2 NuGet package

You can also clone reference projects at a pinned tag into the session scratchpad.

Rules:

- **Prefer the documented or widely adopted approach.** Do something different only
  with a written reason: what the documented approach doesn't cover, and the evidence.
- **No trial and error.** If the first attempt doesn't fix the problem, stop and
  research before a second one. Never stack workarounds; replace the failed one.
- **Write down what you found.** A fix commit ends with a `Prior art:` section: the
  sources read and what they recommend, or "none found", naming where you searched.

## 2. Fix the cause everywhere, not the symptom where it was seen

- Before fixing a bug, find **every place that shares its cause**: other display modes
  (Single, Multi, Compare), time bases (Beats, Seconds), live and frozen, mono and
  stereo, other readouts, the other platform, other input devices.
- Fix the shared helper or rule, not the one call site. If two places compute the same
  thing, make them call one function.
- When you add a tolerance, a guard or a special case to one comparison, check every
  other comparison of the same values: `grep` for the helper and for the values.
- A fix to the Windows web view (`WebViewWin.cpp`) needs its macOS counterpart
  (`WebViewMac.mm`), and the other way round, unless you write down why not.
- The engine is the source of truth. The UI shows what the frame says (lanes, views,
  roll, pan reach, sample rate) and never works it out again from settings.

## 3. Understand a value before you merge, move or change it

Two numbers that look alike are not necessarily the same thing, and a difference that
looks like drift may be deliberate. Before you merge values into one constant, remove a
difference between two copies, or set one value equal to another, write down for each:

- **What it measures:** the quantity (peak, RMS, peak-to-peak, energy ratio), whether
  it is absolute (dBFS) or relative (dB), and its units.
- **What it is for:** display, analysis gate, safety margin, tolerance or limit.
- **Why it has the value it has:** from its comment, the commit that introduced it
  (`git log -S`), or the convention it follows.

Merge only when all three match. Otherwise keep separate named constants, side by side
in `protocol/src/limits.ts`, each with its own comment.

- **Treat a gap between two copies as a question, not a typo.** Find out which side
  was meant to be looser, and why, before making them equal.
- **Allow for floating-point error.** When two sides check the same limit, the side
  that receives a value must tolerate rounding (e.g. `< limit * (1 - 1e-9)`).
- **A derived value needs a stated margin.** A buffer sized "exactly the longest span"
  has no room for audio arriving a block late, rounding, or reads at the edge. Write the
  margin, and why it is enough, next to the constant.
- **A constant's comment must be true.** Check any reason it gives against the numbers
  before committing.

## 4. Audio conventions

- **Levels:** every dB conversion and every displayed level goes through the shared
  helpers and floors in `protocol/src/limits.ts` and `ui/src/view/format.ts`. No
  ad-hoc `log10`, epsilon floors or `toFixed` for levels anywhere else.
- **No value is NaN, never 0.** "Not measured", "silent" and "never sound together"
  travel as NaN and show "—". A real 0 dB or 0 correlation must stay distinguishable
  from "no data".
- **Readouts:** round first, then choose the unit and precision. Never exponent
  notation, never "−0.0". Use U+2212 for minus. Every typed field parses what its
  formatter prints, in its own units only.
- **Thresholds** (silence floors, analysis gates, correlation limits, cancellation
  margins) cite the convention they follow, or are marked in their comment as a
  heuristic.
- **Help text** reads its numbers from `limits.ts` and describes what the code does in
  every mode it applies to.

## 5. Testing

- **A bug fix starts with a test that fails without the fix.** Run it and see it fail
  before you fix.
- **Generate test cases from the source of truth:** the protocol's enums, settings
  table, field table and formatter exports. A new mode, setting, formatter or field is
  then tested as soon as it is added. Prefer parameterized tests: GoogleTest `TEST_P`
  with a name generator, vitest `test.each`.
- **Cover the dimensions the changed code branches on, including the boundaries:**
  - sample rate, from 44.1 to 384 kHz;
  - span lengths on both sides of every tolerance threshold;
  - mono and stereo;
  - live and frozen;
  - success and failure of every read.
- **Every new failure branch** (failed read, missing data, timeout) gets a test that
  triggers it, using a fault-injecting sample source.
- **Capacities are checked at compile time.** Where a quantity must fit a capacity
  (holds, buffers, margins), `static_assert` it at the most demanding supported setting.
- **Analysis is tested against the shared signal corpus** (tones, chords, kick, noise,
  inverted and delayed copies), with numeric acceptance criteria written in the test.
- **After steady input the engine does no repeated work:** test that frames stop
  rebuilding and no lane stays partial.
- **UI behaviour that depends on app state** (marker placement, enabled controls, pan
  limits) gets an app-level test through the mock transport, not just a unit test of a
  helper.
- **Before pushing a fix, check its test catches it:** revert the fix locally and
  confirm the test fails.

## 6. Code rules

- No `std::map`/`unordered_map` `operator[]` for lookups; use `find` or `try_emplace`.
  `operator[]` silently inserts a default entry.
- A cache is marked valid only when every input was read successfully.
- Real-time safety, dependencies and shared numbers: follow CONTRIBUTING.md.
- Match the surrounding code's naming, comment density and idiom.

## 7. Delivering

- **One issue per commit**, each with its test. Don't batch unrelated fixes.
- **Work on a branch and open a PR.** Don't push fixes directly to `main`.
- **Run the checks before pushing:**

  | Check | Command |
  | --- | --- |
  | TypeScript | `npm run typecheck` (`tsc -b`; `tsc --noEmit -p .` checks nothing), `npm test`, `npm run lint`, `npm run format:check` |
  | Everything on macOS | `scripts/check.sh` |
  | Windows code | `scripts\check.ps1` on Windows |

- **Before pushing, have an independent review agent check the diff** against these
  questions:
  - Is there a documented, simpler or widely adopted way?
  - Does the same cause remain anywhere else?
  - Does the diff merge, equalise or derive values? If so, is each one's meaning and
    margin stated?
  - Does every fix have a test that fails without it?
- **Commit messages:** a short imperative subject in plain English; a body saying what
  was wrong, for whom, and what changed; then the `Prior art:` section for fixes
  involving a platform, SDK, host or standard.

## 8. Not in place yet

These rules refer to tools that are still being built. Until each exists, follow the
rule by hand and say so in the commit:

- A fault-injecting sample source for engine tests (section 5).
- The shared signal corpus (section 5).
- An app-level UI test harness using the mock transport (section 5).
- A CI job that runs on every PR (section 7). `.github/workflows/ci.yml` is still
  manual-only.
