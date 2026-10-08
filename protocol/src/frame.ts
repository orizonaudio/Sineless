// Binary layout of a display frame: a little-endian header, then float32 data for each
// view present in `views` (a ChannelView bit mask), in ChannelView order. Each view holds
// `pointCount` points: a min/max pair per point in FrameMode.minMax, one sample per point in
// FrameMode.raw.
//
// Multi-track frames have `laneCount` above 0 and no views: the data holds that many lanes, one
// per track, in the order of the `lanes` message whose `laneSet` matches the frame's.
//
// `samplesPerPoint` is the number of samples each point covers. The visible span starts
// `startFraction` samples after the first point (sub-sample trigger alignment). Readouts cover
// the trigger channel; `frequency` is 0 when unavailable. In a tempo-synced sweep, `cursor` is
// the playhead's position across the screen (0 to 1), and -1 otherwise; `cycleStart` is the
// project position of the left edge, `spanQuarterNotes` the span shown and `barQuarterNotes` the
// bar length, all in quarter notes (0 when not synced).
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
] as const satisfies readonly FrameField[];

export type FrameHeaderFieldName = (typeof frameHeaderFields)[number]["name"];

// Bits of the `flags` field.
// `synced`: a tempo-synced sweep. `limited`: the span was shortened to what the capture holds.
export const FrameFlag = {
  triggered: 1,
  frozen: 2,
  roll: 4,
  synced: 8,
  limited: 16,
} as const;

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
