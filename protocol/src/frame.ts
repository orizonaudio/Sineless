// Binary layout of a display frame: a little-endian header, then float32 data for each
// view present in `views` (a ChannelView bit mask), in ChannelView order. Each view holds
// `pointCount` points: a min/max pair per point in FrameMode.minMax, one sample per point in
// FrameMode.raw.
//
// Multi-track frames have `laneCount` above 0 and no views: the data holds that many lanes, one
// per track, in the order of the `lanes` message whose `laneSet` matches the frame's.
//
// Compare frames (flag `compared`) hold one lane per track and then the sum of the compared
// tracks (those whose lanes entry has `compared`), so `laneCount` is the track count plus one.
// Over the visible part, `sumGain` is the sum's level against the level the same tracks would
// give if unrelated, in dB (0 harmless, below 0 cancelling, -Infinity cancelling exactly; NaN when
// they never sound together), and `fixedGain` the same with every compared track corrected as
// found. Both are raw: the UI's formatter applies the gain floor. After the lane data comes one
// match per track, in lane order, of three float32 values each: the correlation with this track
// (-1 to +1), the offset in samples by which the track lags this track (negative: it leads), and
// its polarity: 1 when it matches this track best as it is, -1 when flipped, 0 when it matches
// nowhere clearly (unrelated sounds; the offset is then NaN). Tracks that are not compared, and
// this track, have no correlation or offset (NaN) and polarity 0; a correlation not measured yet,
// or of tracks that never sound together, is NaN too.
//
// `samplesPerPoint` is the number of samples each point covers. The visible span starts
// `startFraction` samples after the first point (sub-sample trigger alignment). Readouts cover
// the trigger channel; `peak`, `rms` and `frequency` are NaN when not measured (no frequency found,
// or nothing to measure). In a tempo-synced sweep, `cursor` is
// the playhead's position across the screen (0 to 1), and -1 otherwise; `cycleStart` is the
// project position of the left edge, `spanQuarterNotes` the span shown and `barQuarterNotes` the
// bar length, all in quarter notes (0 when not synced); `cycleSeconds` is the left edge's project
// time in seconds.
//
// Samples that are not finite (NaN or infinite, from a broken signal upstream) are measured
// around: left out of the readouts and Compare's matches and gains, and drawn as they are.
// `invalidSamples` is how many this track has in what its readouts measure, and `invalidLanes`
// has bit i set when lane i has any in what the frame shows; in Single, which has no lanes, bit 0
// when any trace does.
//
// View zoom: the frame covers the part `viewStart` to `viewEnd` of the span (fractions;
// 0 to 1 unzoomed), which holds `visibleSamples` samples. Raw point k sits at
// viewStart + (k - startFraction) / visibleSamples * (viewEnd - viewStart); min/max columns
// divide the covered part evenly.

export type FieldType = "u8" | "u16" | "u32" | "f32";

export interface FrameField {
  readonly name: string;
  readonly type: FieldType;
}

export const frameHeaderFields = [
  { name: "sequence", type: "u32" },
  { name: "mode", type: "u8" },
  { name: "flags", type: "u8" },
  { name: "views", type: "u8" },
  { name: "laneCount", type: "u8" },
  { name: "pointCount", type: "u32" },
  { name: "samplesPerPoint", type: "f32" },
  { name: "startFraction", type: "f32" },
  { name: "peak", type: "f32" },
  { name: "rms", type: "f32" },
  { name: "frequency", type: "f32" },
  { name: "cursor", type: "f32" },
  { name: "cycleStart", type: "f32" },
  { name: "spanQuarterNotes", type: "f32" },
  { name: "barQuarterNotes", type: "f32" },
  { name: "viewStart", type: "f32" },
  { name: "viewEnd", type: "f32" },
  { name: "visibleSamples", type: "f32" },
  { name: "laneSet", type: "u32" },
  { name: "sumGain", type: "f32" },
  { name: "fixedGain", type: "f32" },
  { name: "cycleSeconds", type: "f32" },
  { name: "panReach", type: "f32" },
  { name: "invalidSamples", type: "u32" },
  { name: "invalidLanes", type: "u32" },
] as const satisfies readonly FrameField[];

export type FrameHeaderFieldName = (typeof frameHeaderFields)[number]["name"];

// Bits of the `flags` field.
// `synced`: a tempo-synced sweep. `limited`: the span was shortened to what can be kept.
// `partial`: a sweep longer than what the capture held is still filling in its first pass.
export const FrameFlag = {
  triggered: 1,
  frozen: 2,
  roll: 4,
  synced: 8,
  limited: 16,
  compared: 32,
  partial: 64,
} as const;

// Values per track in a Compare frame's matches: correlation, offset and polarity. The
// correlation is NaN when the two tracks never sound together, so nothing was measured.
export const matchValues = 3;

const fieldSizes: Record<FieldType, number> = { u8: 1, u16: 2, u32: 4, f32: 4 };

export interface FieldLayout {
  readonly name: string;
  readonly type: FieldType;
  readonly offset: number;
}

// Offsets follow the field order. Every field must be naturally aligned.
export function layoutFields(fields: readonly FrameField[]): {
  fields: FieldLayout[];
  size: number;
} {
  let offset = 0;
  const laidOut = fields.map((field) => {
    const size = fieldSizes[field.type];
    if (offset % size !== 0) {
      throw new Error(
        `Frame field '${field.name}' is not aligned to ${String(size)} bytes`,
      );
    }
    const layout = { name: field.name, type: field.type, offset };
    offset += size;
    return layout;
  });
  return { fields: laidOut, size: offset };
}

export const frameHeader = layoutFields(frameHeaderFields);
