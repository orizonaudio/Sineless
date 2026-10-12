// Numbers the engine and the UI must agree on. Each is defined here once; the C++ header is
// generated from this list (see cpp-generator.ts), so neither side keeps a copy of its own.
import { settings } from "./settings.ts";

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
// tracks there, and the frequency readout finds no pitch there. 16-bit dither falls below it.
export const analysisGateDbfs = -90;

// How a signal's cycles are found, for its frequency: where it rises through its mean level,
// after first falling this fraction of its peak-to-peak range below it, so noise riding on a
// crossing does not count twice.
export const crossingHysteresis = 0.1;

// A signal repeats steadily when most of its cycles last within this fraction of the median
// cycle; otherwise it has no frequency to show (noise, or several pitches at once).
export const periodTolerance = 0.15;

// Seconds of audio a freeze holds in Seconds mode: the longest span, which a frozen view can be
// panned back through. The engine's capture is sized to keep it at 384 kHz, the highest rate hosts
// run at (CaptureRing::capacityFor; the build checks it), so a longer hold needs a larger one.
export const frozenHoldSeconds = settings.timeSpan.maximum;

// Where a triggered trace's trigger point sits, as a fraction of the span from the left.
export const triggerPosition = 0.1;

// Spans longer than this (seconds) scroll continuously instead of triggering.
export const rollThresholdSeconds = 1;

// The narrowest part of the span a view can zoom in to.
export const minimumViewWidth = 1 / 256;

// The most tracks the multi-track view shows; the Compare view shows one fewer, for the sum.
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

// How much of the newest audio the frequency readout measures (seconds).
export const frequencyWindowSeconds = 1;

// The editor's layout (CSS pixels): its minimum width; the minimum height of a stacked lane; and
// the height of everything around the screen (top bar and controls), at most: measured at 142,
// with room for rounding at other display scales. The editor tests check the page against it.
export const minimumEditorWidth = 900;
export const minimumLaneHeight = 40;
export const chromeHeight = 144;

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
  { name: "triggerPosition", value: triggerPosition, kind: "double" },
  { name: "rollThresholdSeconds", value: rollThresholdSeconds, kind: "double" },
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
  {
    name: "frequencyWindowSeconds",
    value: frequencyWindowSeconds,
    kind: "double",
  },
  { name: "minimumEditorWidth", value: minimumEditorWidth, kind: "pixels" },
  { name: "minimumLaneHeight", value: minimumLaneHeight, kind: "pixels" },
  { name: "chromeHeight", value: chromeHeight, kind: "pixels" },
];
