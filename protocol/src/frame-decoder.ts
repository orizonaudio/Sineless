// Decodes a display frame encoded in the layout of frame.ts (little-endian).
import { ChannelView, FrameMode } from "./enums.ts";
import { FrameFlag, frameHeader, type FrameHeaderFieldName } from "./frame.ts";

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
  // Playhead position across the screen in a tempo-synced sweep (FR-9).
  readonly cursor: number | undefined;
  // Musical context of a synced sweep, in quarter notes (FR-9).
  readonly cycleStart: number;
  readonly spanQuarterNotes: number;
  readonly barQuarterNotes: number;
  // The part of the span this frame covers, and its length in samples (FR-10).
  readonly viewStart: number;
  readonly viewEnd: number;
  readonly visibleSamples: number;
  // Points of each view present, keyed by ChannelView; min/max pairs in FrameMode.minMax.
  readonly views: ReadonlyMap<ChannelView, Float32Array>;
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

  const views = new Map<ChannelView, Float32Array>();
  let offset = frameHeader.size;
  for (const view of Object.values(ChannelView)) {
    if ((viewMask & (1 << view)) === 0) {
      continue;
    }
    if (offset + valuesPerView * 4 > buffer.byteLength) {
      throw new Error("Frame data is shorter than its header announces");
    }
    views.set(view, new Float32Array(buffer, offset, valuesPerView));
    offset += valuesPerView * 4;
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
    frequency: frequency > 0 ? frequency : undefined,
    synced: (flags & FrameFlag.synced) !== 0,
    limited: (flags & FrameFlag.limited) !== 0,
    cursor: cursor >= 0 ? cursor : undefined,
    cycleStart: f32("cycleStart"),
    spanQuarterNotes: f32("spanQuarterNotes"),
    barQuarterNotes: f32("barQuarterNotes"),
    viewStart: f32("viewStart"),
    viewEnd: f32("viewEnd"),
    visibleSamples: f32("visibleSamples"),
    views,
  };
}
