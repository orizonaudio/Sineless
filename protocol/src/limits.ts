// Numbers the engine and the UI must agree on. Each is defined here once; the C++ header is
// generated from this list (see cpp-generator.ts), so neither side keeps a copy of its own.
import { settings } from "./settings.ts";

// The level, in dBFS, at or below which audio counts as silence: readouts show minus infinity,
// and analysis leaves it out. Only silence, dither or a cancelled sum's residue is that quiet.
export const silenceFloorDb = -100;

// How a signal's cycles are found, for its frequency: where it rises through its mean level,
// after first falling this fraction of its peak-to-peak range below it, so noise riding on a
// crossing does not count twice.
export const crossingHysteresis = 0.1;

// A signal repeats steadily when most of its cycles last within this fraction of the median
// cycle; otherwise it has no frequency to show (noise, or several pitches at once).
export const periodTolerance = 0.15;

// Seconds of audio a freeze holds in Seconds mode: the longest span, which a frozen view can be
// panned back through.
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
  { name: "silenceFloorDb", value: silenceFloorDb, kind: "double" },
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
