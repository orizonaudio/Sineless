---
name: frame-timing
description: Diagnose motion that stutters, judders, jumps, flickers or flashes, frames dropped or repeated, or uneven sweeps and playheads, in any mode, host or display, by measuring what reaches the screen before naming a cause. Use on any such report or suspicion, before planning a fix for one, and when judging whether a fix for one worked.
---

# Frame timing: measure the screen first

From 28 October to 3 November 2026 six attempts went at the traces' stutter before its cause was
found: WebKit's commits through the host's main thread repeated about one refresh in five. Every
wrong diagnosis rested on evidence from before the screen (engine logs, the page's
`requestAnimationFrame` times, pixels read back from its canvas, simulations, tests of a model of
the page), while the screen did something else. What found it was a frame barcode read back from
screen recordings, with a control in the same window. This skill is that method.

The chain from a frame to the eye: engine → page or native layer draws a picture → (WebKit and
the host's main thread, in WebGL mode) → WindowServer → display. Each link's own clock or log is
evidence about that link only. Say which link every claim measured.

## 1. Measure the screen first

No cause is named and no fix planned until a recording with the frame barcode says where pictures
are lost:

- **not drawn for a refresh** ("repeat, nothing drawn"): making, or nothing new to draw (frozen,
  Waiting, nothing changed; natively a frame held for n refreshes is drawn once, for the first);
- **drawn, then shown late or never** ("presented late", "pictures never shown"): presentation;
- **nowhere**: every refresh a new picture on time, yet the motion looks uneven: then the positions
  within the pictures are wrong (look at the engine's placement), not the frames.

How:

1. Turn the switch on for the host and restart it:
   `launchctl setenv SINELESS_FRAME_BARCODE 1` (turn off with `launchctl unsetenv`). In the
   browser preview, `?barcode`. Each plugin window shows a small strip at the screen's top right;
   no strip, no switch.
2. With the transport running and the host in front:
   `FRAME_BARCODE_DISPLAY=external scripts/frame-barcode/record.sh 60` (or `builtin`; the main
   display by default). It finds the strips, records one grey per cell per refresh for 60 s and
   prints the counts per strip. A QuickTime or ffmpeg movie also reads:
   `node scripts/frame-barcode/analyse.ts movie.mov` (its idle refreshes are unknown).
3. Read the table: per strip, new on time or late; repeats with nothing drawn (making) or with a
   picture drawn (presentation); pictures never shown; and, with several strips, the refreshes
   where they repeated together (a cause they share: the host, WindowServer).

Natively, `perf_bench` also counts from Metal's presented times (frames on screen, refreshes
without a new frame, dropped drawables). Its counts agreed with the barcode on steady runs; until a
stuttering run has been compared, treat them as a cross-check, not a replacement.

## 2. Controls

- A control in the same window and recording: the AU and the VST3 together, another scope, or
  a build before the change. Two instances repeating at the same refreshes point to what they
  share.
- One change per comparison, everything else the same: display, host, buffer, other windows.
- A baseline measured at least twice, its spread stated, before any change is judged by it (the
  repeat rate swung from 3% to 24% between minutes one day).
- Keep an untouched "before" build: `scripts/check.sh` installs this checkout's AU over the one in
  `~/Library/Audio/Plug-Ins/Components`.

## 3. Hypotheses

- Each cause states what the counts will do if it holds, and what would refute it, before it is
  tested. Nothing is "found" until the counts move as predicted.
- A cause given up is written down with the measurement that refuted it.
- Brendan Gregg's anti-methods apply: not the tools at hand (Streetlight), not one setting after
  another (Random Change).

## 4. Setup checklist

- Which display: the built-in Retina or the external 1× one, and its refresh rate. Problems seen
  on one may not show on the other (the playhead flashed on the 1× display only).
- Other windows visible, remote-desktop software (AnyDesk) closed, the screen unlocked
  (`perf_bench` and WebKit draw nothing while it is locked).
- The host in front: Live hides plugin windows when it is not. A new plugin binary loads only once
  the host restarts.
- Transport running for motion; stopped or frozen, nothing new is drawn, so repeats with nothing
  drawn are expected.
- The pointer, cursors and the hover line off the strip: a crossed cell reads unreadable.

## 5. Tools and their costs

- **The recorder itself** records every refresh with no frame-interval limit (none missed in two
  30 s runs, 2026-11-03). Its gaps are counted unknown, never as stutter; gaps in a recording mean
  the recorder fell behind. A limit of 1/rate brings them back, about one refresh in 20 at 60 Hz;
  why so many is not known (the display's period is only 41 to 290 ns under 1/60 s).
- **Instruments** only after the barcode has located the loss, to see why. The Animation Hitches
  template adds a hitch of its own about every 2 s: compare with it running and not. All-process
  templates wrote about 90 MB/s and filled the disk once: check `df`, size the trace from a short
  dry run, prefer Metal System Trace or a fixed 15 s window, and delete `instruments*.ktrace` after
  export.
- **Logging** on the host's main thread changes its timing; log off it, or not at all.
- **Joining sources by time:** the recorder's display times and the native link's target times
  keep a constant offset (lateness within 0.4 ms over a minute); the page's animation-frame times
  are not compared yet. Perfetto puts several sources on one timeline.
- **Windows:** the same method, with a recording on a real PC; never the Parallels VM, whose
  timing is not the hardware's.

## 6. Tests

For a presentation fault (pictures drawn on time, shown late or never), the failing reproduction
is a recording before and after the fix, its counts in the commit message and the pull request:
CI's reviewers do not see the local plan. Unit tests cover the mechanism the fix adds and say what
they cannot see. A making fault still gets a unit or app test that fails without the fix.

## Sources

- Apple, Tech Talk 10855 "Explore UI animation hitches and the render loop": "A hitch is any
  time a frame appears on screen later than expected."
- web.dev, "Towards an animation smoothness metric": polling `requestAnimationFrame` "suggested a
  frame drop to 0, but visually, a user wouldn't be able to notice a difference!"
- NVIDIA FCAT (PCPer, "Frame Rating Dissected"): a per-frame code read from captured video; "Dropping
  a frame on the capture side would show up as dropped frames from the graphics card".
- PresentMon (GameTechDev/PresentMon, `README-ConsoleApplication.md`): presented against displayed
  frames.
- Apple, MTLDrawable `presentedTime`: "0.0 if the drawable hasn't been presented or if its
  associated frame was dropped."
- Brendan Gregg, Methodology and Active Benchmarking.
