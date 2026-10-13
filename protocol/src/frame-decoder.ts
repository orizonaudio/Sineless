// Decodes a display frame encoded in the layout of frame.ts (little-endian).
import { ChannelView, FrameMode } from "./enums.ts";
import {
  FrameFlag,
  frameHeader,
  matchValues,
  type FrameHeaderFieldName,
} from "./frame.ts";

export interface Frame {
  readonly sequence: number;
  readonly mode: FrameMode;
  readonly triggered: boolean;
  readonly frozen: boolean;
  readonly roll: boolean;
  readonly pointCount: number;
  readonly samplesPerPoint: number;
  readonly startFraction: number;
  readonly peak: number;
  readonly rms: number;
  readonly frequency: number | undefined;
  readonly synced: boolean;
  readonly limited: boolean;
  readonly partial: boolean; // The sweep's first pass is still filling in.
  // Playhead position across the screen in a tempo-synced sweep.
  readonly cursor: number | undefined;
  // Musical context of a synced sweep, in quarter notes.
  readonly cycleStart: number;
  readonly cycleSeconds: number; // The left edge's project time.
  // Frozen in Seconds mode: how far back the view can be panned (seconds); 0 otherwise.
  readonly panReach: number;
  // Samples not finite (measured around): this track's count in what its readouts measure, and a
  // bit per lane holding any in what the frame shows.
  readonly invalidSamples: number;
  readonly invalidLanes: number;
  readonly spanQuarterNotes: number;
  readonly barQuarterNotes: number;
  // The part of the span this frame covers, and its length in samples.
  readonly viewStart: number;
  readonly viewEnd: number;
  readonly visibleSamples: number;
  // Points of each view present, keyed by ChannelView; min/max pairs in FrameMode.minMax.
  readonly views: ReadonlyMap<ChannelView, Float32Array>;
  // Multi-track frames: one lane per track instead of views, described by the lanes message
  // with the same laneSet.
  readonly lanes: readonly Float32Array[];
  readonly laneSet: number;
  // Compare frames: the analysis of the compared tracks; their sum is the last lane.
  readonly comparison: Comparison | undefined;
}

export interface Comparison {
  readonly sumGain: number;
  readonly fixedGain: number;
  // One per track lane, in lane order; zeros for tracks not compared and for this track.
  readonly matches: readonly Match[];
}

// How a track lines up with this track. Unrelated tracks match nowhere clearly: not `related`,
// with no offset and not inverted.
export interface Match {
  readonly correlation: number;
  readonly related: boolean;
  readonly offset: number; // Samples the track lags this track (negative: it leads).
  readonly inverted: boolean;
}

const offsets = Object.fromEntries(
  frameHeader.fields.map((field) => [field.name, field.offset]),
) as Record<FrameHeaderFieldName, number>;

export function decodeFrame(buffer: ArrayBuffer): Frame {
  if (buffer.byteLength < frameHeader.size) {
    throw new Error(
      `Frame of ${String(buffer.byteLength)} bytes is shorter than its header`,
    );
  }
  const header = new DataView(buffer, 0, frameHeader.size);
  const u8 = (name: FrameHeaderFieldName): number =>
    header.getUint8(offsets[name]);
  const u32 = (name: FrameHeaderFieldName): number =>
    header.getUint32(offsets[name], true);
  const f32 = (name: FrameHeaderFieldName): number =>
    header.getFloat32(offsets[name], true);

  const mode = u8("mode") as FrameMode;
  const flags = u8("flags");
  const viewMask = u8("views");
  const pointCount = u32("pointCount");
  const valuesPerView = pointCount * (mode === FrameMode.minMax ? 2 : 1);

  let offset = frameHeader.size;
  const next = (): Float32Array => {
    if (offset + valuesPerView * 4 > buffer.byteLength) {
      throw new Error("Frame data is shorter than its header announces");
    }
    const values = new Float32Array(buffer, offset, valuesPerView);
    offset += valuesPerView * 4;
    return values;
  };
  const views = new Map<ChannelView, Float32Array>();
  for (const view of Object.values(ChannelView)) {
    if ((viewMask & (1 << view)) !== 0) {
      views.set(view, next());
    }
  }
  const lanes = Array.from({ length: u8("laneCount") }, next);
  const compared = (flags & FrameFlag.compared) !== 0;
  const matches: Match[] = [];
  if (compared) {
    const count = Math.max(lanes.length - 1, 0);
    if (offset + count * matchValues * 4 > buffer.byteLength) {
      throw new Error("Frame data is shorter than its header announces");
    }
    const values = new Float32Array(buffer, offset, count * matchValues);
    for (let i = 0; i < count; i++) {
      const polarity = values[i * matchValues + 2] ?? 0;
      matches.push({
        correlation: values[i * matchValues] ?? 0,
        related: polarity !== 0,
        offset: values[i * matchValues + 1] ?? 0,
        inverted: polarity < 0,
      });
    }
  }

  const frequency = f32("frequency");
  const cursor = f32("cursor");
  return {
    sequence: u32("sequence"),
    mode,
    triggered: (flags & FrameFlag.triggered) !== 0,
    frozen: (flags & FrameFlag.frozen) !== 0,
    roll: (flags & FrameFlag.roll) !== 0,
    pointCount,
    samplesPerPoint: f32("samplesPerPoint"),
    startFraction: f32("startFraction"),
    peak: f32("peak"),
    rms: f32("rms"),
    frequency: Number.isNaN(frequency) ? undefined : frequency,
    synced: (flags & FrameFlag.synced) !== 0,
    limited: (flags & FrameFlag.limited) !== 0,
    partial: (flags & FrameFlag.partial) !== 0,
    cursor: cursor >= 0 ? cursor : undefined,
    cycleStart: f32("cycleStart"),
    cycleSeconds: f32("cycleSeconds"),
    panReach: f32("panReach"),
    invalidSamples: u32("invalidSamples"),
    invalidLanes: u32("invalidLanes"),
    spanQuarterNotes: f32("spanQuarterNotes"),
    barQuarterNotes: f32("barQuarterNotes"),
    viewStart: f32("viewStart"),
    viewEnd: f32("viewEnd"),
    visibleSamples: f32("visibleSamples"),
    views,
    lanes,
    laneSet: u32("laneSet"),
    comparison: compared
      ? { sumGain: f32("sumGain"), fixedGain: f32("fixedGain"), matches }
      : undefined,
  };
}
