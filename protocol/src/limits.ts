// Numbers the engine and the UI must agree on. Each is defined here once; the C++ header is
// generated from this list (see cpp-generator.ts), so neither side keeps a copy of its own.

// Three different "too quiet" limits, for three different quantities. For scale: 16-bit dither is
// about -96 dBFS RMS, converter and analog noise floors -110 to -120, 24-bit dither about -144,
// and the residue of a polarity flip after a float gain round trip about -150.
//
// Level readouts (peak, RMS, hover, level cursors) show minus infinity at or below this level
// (dBFS): converter noise floors still read as numbers; 24-bit dither and digital residue do not.
export const levelFloorDbfs = -120;

// A summing gain (a ratio: the sum against unrelated tracks of the same levels, in dB) shows
// minus infinity at or below this: a deep but real null shows its depth, while what is left of
// an exact cancellation after float rounding reads as none. The engine sends gains unfloored (an
// exact cancellation as -Infinity); only the UI's formatter applies this.
export const gainFloorDb = -140;

// Analysis leaves out audio whose RMS level (dBFS) is at or below this: Compare does not match
// tracks there, and the hover readout finds no cycle there. 16-bit dither falls below it.
export const analysisGateDbfs = -90;

// How the hover readout finds a signal's cycles: where it rises through its mean level,
// after first falling this fraction of its peak-to-peak range below it, so noise riding on a
// crossing does not count twice.
export const crossingHysteresis = 0.1;

// A cycle under the hover is steady when a neighbouring cycle lasts within this fraction of it;
// otherwise it has no frequency to show (noise, or several pitches at once).
export const periodTolerance = 0.15;

// Seconds a frozen Trigger or Free view can be panned back through: the longest span, Free's
// longest (settings.freeSpan takes its maximum from here). The freeze holds
// twice pacingDelayCapSeconds more, as the paced edge may stand that far behind the newest audio
// (kSecondsFreezeSeconds). The engine's capture is sized to keep that at 384 kHz, the highest
// rate hosts run at (CaptureRing::capacityFor; the build checks it), so a longer hold needs a
// larger one.
export const frozenHoldSeconds = 4;

// Where the trigger point sits unless the Trigger Position setting says otherwise, as a fraction
// of the span from the left: the setting's default, and where every trigger sat before it was a
// setting, so old projects open as they were. A tenth leaves room to see what led up to the edge
// while most of the screen shows what follows it.
export const defaultTriggerPosition = 0.1;

// The longest span of the Trigger time base (seconds). Longer, a trigger would hold one trace for
// a long time and show little of the signal's change (Auto's wait for an edge grows with the
// span too); the Free time base shows longer spans moving instead.
export const longestTriggerSpanSeconds = 1;

// The shortest span of the Free time base (seconds). A free-running window moves one refresh's
// worth of audio each frame: 16.7 ms at 60 Hz, a sixth of the screen at 100 ms. Shorter, each
// refresh shows a different slice of the waveform, which cannot be read; the trigger is what
// makes a picture there.
export const shortestFreeSpanSeconds = 0.1;

// The most a free-running window's edge trails the newest audio (seconds). Hosts deliver audio in
// blocks, and frames come at display rate; an edge at the newest sample moves by one block on some
// frames and two on others. The edge is paced instead: a steady clock estimates the sample due now
// and the edge stays a measured delay behind it, about a block. The cap covers 4096-sample blocks
// at 44.1 kHz (93 ms), the longest hosts commonly use, with a third more for late deliveries; a
// host whose blocks come further apart steps as before. A freeze holds twice this beyond the
// longest span, so the frozen window is the one last shown.
export const pacingDelayCapSeconds = 0.125;

// Spans shorter than this (seconds) are not paced: below about one frame (11 to 20 ms, as the page
// draws 50 to 90 frames a second) the whole trace is new every frame, and pacing would only show
// it later.
export const pacedSpanMinimumSeconds = 0.02;

// The narrowest part of the span a view can zoom in to.
export const minimumViewWidth = 1 / 256;

// The most tracks the multi-track view shows; with Compare on it shows one fewer, for the sum.
export const maxLanes = 16;

// How often readouts are measured and refreshed (seconds): legible while frames arrive faster.
export const readoutIntervalSeconds = 0.1;

// How far apart two tracks may be for Compare to find their offset (seconds, either way), and how
// much audio before the playhead it compares them over.
export const matchReachSeconds = 0.01;
export const matchWindowSeconds = 8;

// Tracks cancel when their sum is more than this many dB quieter than the same tracks would sum
// if unrelated (the summing gain), and reinforce when it is that much louder; within it, summing
// is harmless.
export const summingMarginDb = 1;

// The audible range (Hz): a frequency outside it, from the time cursors or the hover,
// reads "—" and has no note; below 20 Hz a "frequency" is a slow change, above 20 kHz an artefact.
export const lowestAudibleHz = 20;
export const highestAudibleHz = 20000;

// The editor's layout (CSS pixels): its minimum width, the wider of the widest toolbar (Trig, the
// Single trigger mode and frozen, in Single or in Multi with Compare on: one width since Single's
// and Multi's Views share a place; measured at 997 with the wordmark) and the status bar, whose
// parts keep fixed widths in every state (measured at 1136 with the Grid field, in WebKit and
// Chromium, docs/ui/README.md), with at least 8 px to spare for text that renders wider on another
// platform, rounded up to 10; its default width,
// the same, as nothing in the window changes with its width; its minimum height, which fits the
// tallest panel in the screen's corner (Time cursors in Beats, Level cursors and the Meter, measured
// at 315 px from 9 px below the screen's top) with at least 8 px to spare, rounded up to 10;
// the minimum height of a stacked lane; and the height of everything around the screen (toolbar
// and status bar), at most: measured at 82, with room for rounding at other display scales. The
// editor tests check the page against them.
export const defaultEditorWidth = 1150;
export const minimumEditorWidth = 1150;
export const minimumEditorHeight = 420;
export const minimumLaneHeight = 40;
export const chromeHeight = 84;

// Frames are drawn on every n-th display refresh, n the smallest whole number keeping the refresh
// rate divided by n at or below this (the page counts requestAnimationFrame ticks, the macOS editor
// its display link's refreshes), so each frame stays on screen for the same n ticks (#308): a
// frame held for 2, 1, 2 ticks while the playhead moves by the time between frames judders.
// Every tick up to 90 Hz, 50 frames a second at 100 Hz, 72 at 144 Hz. 95 Hz lies between the
// common rates 90 and 100 Hz, and its multiples (190, 285, 380 Hz) between theirs, so a measured
// rate a little off never puts a common display on the wrong side.
export const fullRateUpToHz = 95;
// Once n is set, it changes only when the measured rate is this share past a line, so a rate
// measured near one does not change n back and forth. The common rates lie further from the
// lines (5%).
export const lineHysteresis = 0.02;

// The page's frame around the screen (CSS pixels): the toolbar's and the status bar's heights, the
// screen's margin at each side, its border and its corner radius. The page's stylesheet takes them
// at start-up, and the macOS editor works the screen's rectangle out from them, so the traces it
// draws natively sit exactly in the page's screen. The design tokens hold the same sizes
// (docs/ui/tokens/sineless-tokens.json: toolbar-height, status-height, screen-inset, screen-radius).
export const toolbarHeight = 48;
export const statusBarHeight = 34;
export const screenMargin = 8;
export const screenBorder = 1;
export const screenRadius = 10;

// The screen's grid, which the page and the macOS editor both draw.
//
// Time, in Trig and Free: the screen's width in gridColumnsCoarse, gridColumnsMedium or
// gridColumnsFine equal divisions by the Time step. Time labels sit on its fifths
// (timeLabelColumns), which every step's divisions include: four labels at each step, on the
// lines Medium labels, neither crowded at Fine nor thinned at Coarse.
export const gridColumnsCoarse = 5;
export const gridColumnsMedium = 10;
export const gridColumnsFine = 20;
export const timeLabelColumns = 5;

// Level: lines every gridLevelStepDb down from the top of each lane, at most gridLevelsBelowTop
// below it (the trace is linear, so a line d dB down sits at 10^(-d / 20) of the half-lane), at
// every Level step. Coarse draws every gridCoarseLevelStride-th of them (6 dB); Fine adds
// gridFineLevelSubdivisions - 1 minor lines inside each step (every 1 dB).
export const gridLevelStepDb = 3;
export const gridLevelsBelowTop = 4;
export const gridCoarseLevelStride = 2;
export const gridFineLevelSubdivisions = 3;
// The closest (CSS pixels) drawn level lines come at each Level step before a short lane drops
// some. Medium's 3 dB lines thin to 6 dB, then to the top only, then to none; Coarse's 6 dB lines
// to the top only, then to none. Fine's minor 1 dB lines may come closer than the major lines,
// being minor and drawn only between two of them; those nearest zero, the narrowest, drop first.
// Coarse's and Medium's are equal but kept apart: each is the spacing of its own lines.
export const gridCoarseLineGap = 8;
export const gridMediumLineGap = 8;
export const gridFineLineGap = 6;
// Beats at the Fine Time step draws one ladder level finer than Medium only where those lines stay
// this far apart (CSS pixels); closer, it draws Medium's. A time spacing, kept apart from the
// level gaps.
export const gridFineBeatLineGap = 8;
// A Cross hair tick's length (CSS pixels), centred on the line it marks: long enough to read at 1x,
// short enough not to read as a line.
export const gridTickLength = 6;

// Compare's cancellation shading, which the page and the macOS editor both draw. Where the sum's
// peak falls more than cancellationDb below the louder compared track's, they cancel strongly.
// This is not the summing gain readout's rule (energy against unrelated tracks, ±1 dB), and cannot
// be: a column holds only peaks, and the peak of two unrelated signals' sum wanders well below what
// their powers add to (on 10 to 30% of columns of noise), which would shade unrelated tracks.
// Against the louder track alone, with 3 dB, unrelated tracks are left unshaded. Stretches quieter
// than cancellationQuietShare of the loudest moment (about -26 dB) cancel nothing audible; gaps of
// up to cancellationBridgedPoints inside a cancelling stretch belong to it; raw points, which
// cross zero every cycle, are compared over a window of a cancellationReachDivisor-th of the frame
// either side.
export const cancellationDb = -3;
export const cancellationQuietShare = 0.05;
export const cancellationBridgedPoints = 3;
export const cancellationReachDivisor = 128;

// What the page and the macOS editor draw fainter than the live picture: while Normal mode waits
// for a trigger, the last traces, the playhead and the shading at waitingAlpha of their opacity, so
// the picture reads as old but stays legible; in Compare, tracks left out of the comparison at
// leftOutAlpha of theirs, so the compared tracks and their sum stand out.
export const waitingAlpha = 0.45;
export const leftOutAlpha = 0.2;

// Musical positions worked out in floating point (a step's multiples, a view's edges) count as
// whole, or as on an edge, within musicalTolerance quarter notes or steps: far above the rounding
// of a sum of steps, far below the finest grid step (a 1/1024 note).
export const musicalTolerance = 1e-6;

// The frame barcode, drawn on every picture while the diagnostic switch is on and read back from
// screen recordings to count, per refresh, pictures shown, repeated and lost. Each row is a white
// and a black reference cell (the decoder takes its threshold from the picture itself, under
// scaling, colour management and compression), barcodeDataBits value cells, most significant
// first, white for 1, an even parity cell (a misread cell fails it, so a row is rejected rather
// than guessed) and a black end cell (a clipped row reads unreadable, not as a smaller value). 16
// bits wrap after 65,536 pictures, 18 minutes at 60 frames/s, and the drawn-for time, counted in
// barcodeTicksPerMillisecond, after 16.4 s: both far longer than a picture is ever late. A quarter
// millisecond resolves lateness well inside the half refresh it is judged by (2 ms at 240 Hz), and
// is the finest step that still wraps after more than 16 s, so a lateness is unwrapped across 8 s
// either way (to the nearest value within half the range). barcodeCellPixels is a cell's side in device pixels: 4 decoded on every refresh of
// recordings of the owner's external 1x and built-in Retina displays (2026-11-03).
export const barcodeDataBits = 16;
export const barcodeTicksPerMillisecond = 4;
export const barcodeCellPixels = 4;
// The strip's margin beyond the screen's rounded corner (CSS pixels): it sits screenRadius plus
// this from the screen's top and right edges, so the corner's clip and its anti-aliased edge never
// reach a cell. The page and the macOS editor draw it in the same place, which the recordings'
// locator and the native and page picture comparison rely on.
export const barcodeCornerMargin = 2;

type Kind = "double" | "count" | "pixels";

// The list the C++ header is generated from.
export const limits: readonly {
  readonly name: string;
  readonly value: number;
  readonly kind: Kind;
}[] = [
  { name: "levelFloorDbfs", value: levelFloorDbfs, kind: "double" },
  { name: "gainFloorDb", value: gainFloorDb, kind: "double" },
  { name: "analysisGateDbfs", value: analysisGateDbfs, kind: "double" },
  { name: "crossingHysteresis", value: crossingHysteresis, kind: "double" },
  { name: "periodTolerance", value: periodTolerance, kind: "double" },
  { name: "frozenHoldSeconds", value: frozenHoldSeconds, kind: "double" },
  {
    name: "defaultTriggerPosition",
    value: defaultTriggerPosition,
    kind: "double",
  },
  {
    name: "longestTriggerSpanSeconds",
    value: longestTriggerSpanSeconds,
    kind: "double",
  },
  {
    name: "shortestFreeSpanSeconds",
    value: shortestFreeSpanSeconds,
    kind: "double",
  },
  {
    name: "pacingDelayCapSeconds",
    value: pacingDelayCapSeconds,
    kind: "double",
  },
  {
    name: "pacedSpanMinimumSeconds",
    value: pacedSpanMinimumSeconds,
    kind: "double",
  },
  { name: "minimumViewWidth", value: minimumViewWidth, kind: "double" },
  { name: "maxLanes", value: maxLanes, kind: "count" },
  {
    name: "readoutIntervalSeconds",
    value: readoutIntervalSeconds,
    kind: "double",
  },
  { name: "matchReachSeconds", value: matchReachSeconds, kind: "double" },
  { name: "matchWindowSeconds", value: matchWindowSeconds, kind: "double" },
  { name: "summingMarginDb", value: summingMarginDb, kind: "double" },
  { name: "defaultEditorWidth", value: defaultEditorWidth, kind: "pixels" },
  { name: "minimumEditorWidth", value: minimumEditorWidth, kind: "pixels" },
  { name: "minimumEditorHeight", value: minimumEditorHeight, kind: "pixels" },
  { name: "minimumLaneHeight", value: minimumLaneHeight, kind: "pixels" },
  { name: "chromeHeight", value: chromeHeight, kind: "pixels" },
  { name: "fullRateUpToHz", value: fullRateUpToHz, kind: "double" },
  { name: "lineHysteresis", value: lineHysteresis, kind: "double" },
  { name: "toolbarHeight", value: toolbarHeight, kind: "pixels" },
  { name: "statusBarHeight", value: statusBarHeight, kind: "pixels" },
  { name: "screenMargin", value: screenMargin, kind: "pixels" },
  { name: "screenBorder", value: screenBorder, kind: "pixels" },
  { name: "screenRadius", value: screenRadius, kind: "pixels" },
  { name: "gridColumnsCoarse", value: gridColumnsCoarse, kind: "count" },
  { name: "gridColumnsMedium", value: gridColumnsMedium, kind: "count" },
  { name: "gridColumnsFine", value: gridColumnsFine, kind: "count" },
  { name: "timeLabelColumns", value: timeLabelColumns, kind: "count" },
  { name: "gridLevelStepDb", value: gridLevelStepDb, kind: "double" },
  { name: "gridLevelsBelowTop", value: gridLevelsBelowTop, kind: "count" },
  {
    name: "gridCoarseLevelStride",
    value: gridCoarseLevelStride,
    kind: "count",
  },
  {
    name: "gridFineLevelSubdivisions",
    value: gridFineLevelSubdivisions,
    kind: "count",
  },
  { name: "gridCoarseLineGap", value: gridCoarseLineGap, kind: "double" },
  { name: "gridMediumLineGap", value: gridMediumLineGap, kind: "double" },
  { name: "gridFineLineGap", value: gridFineLineGap, kind: "double" },
  { name: "gridFineBeatLineGap", value: gridFineBeatLineGap, kind: "double" },
  { name: "gridTickLength", value: gridTickLength, kind: "double" },
  { name: "cancellationDb", value: cancellationDb, kind: "double" },
  { name: "waitingAlpha", value: waitingAlpha, kind: "double" },
  { name: "barcodeDataBits", value: barcodeDataBits, kind: "count" },
  {
    name: "barcodeTicksPerMillisecond",
    value: barcodeTicksPerMillisecond,
    kind: "count",
  },
  { name: "barcodeCellPixels", value: barcodeCellPixels, kind: "count" },
  { name: "barcodeCornerMargin", value: barcodeCornerMargin, kind: "pixels" },
  { name: "leftOutAlpha", value: leftOutAlpha, kind: "double" },
  { name: "musicalTolerance", value: musicalTolerance, kind: "double" },
  {
    name: "cancellationQuietShare",
    value: cancellationQuietShare,
    kind: "double",
  },
  {
    name: "cancellationBridgedPoints",
    value: cancellationBridgedPoints,
    kind: "count",
  },
  {
    name: "cancellationReachDivisor",
    value: cancellationReachDivisor,
    kind: "count",
  },
];
