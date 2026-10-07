// Encodes a display frame in the layout of frame.ts. The engine encodes frames in C++; this
// encoder serves the UI's mock back end and tests.
import { ChannelView, type FrameMode } from "./enums.ts";
import { FrameFlag, frameHeader, type FrameHeaderFieldName } from "./frame.ts";

export interface FrameContent {
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
  readonly synced?: boolean;
  readonly limited?: boolean;
  readonly cursor?: number | undefined;
  readonly cycleStart?: number;
  readonly spanQuarterNotes?: number;
  readonly barQuarterNotes?: number;
  readonly viewStart?: number;
  readonly viewEnd?: number;
  readonly visibleSamples?: number;
  readonly views: ReadonlyMap<ChannelView, ArrayLike<number>>;
}

const offsets = Object.fromEntries(
  frameHeader.fields.map((field) => [field.name, field.offset]),
) as Record<FrameHeaderFieldName, number>;

export function encodeFrame(content: FrameContent): ArrayBuffer {
  const ordered = Object.values(ChannelView).filter((view) =>
    content.views.has(view),
  );
  const dataLength = ordered.reduce<number>(
    (sum, view) => sum + (content.views.get(view)?.length ?? 0),
    0,
  );
  const buffer = new ArrayBuffer(frameHeader.size + dataLength * 4);
  const header = new DataView(buffer, 0, frameHeader.size);
  const flags =
    (content.triggered ? FrameFlag.triggered : 0) |
    (content.frozen ? FrameFlag.frozen : 0) |
    (content.roll ? FrameFlag.roll : 0) |
    (content.synced === true ? FrameFlag.synced : 0) |
    (content.limited === true ? FrameFlag.limited : 0);
  header.setUint32(offsets.sequence, content.sequence, true);
  header.setUint8(offsets.mode, content.mode);
  header.setUint8(offsets.flags, flags);
  header.setUint8(
    offsets.views,
    ordered.reduce<number>((mask, view) => mask | (1 << view), 0),
  );
  header.setUint8(offsets.reserved, 0);
  header.setUint32(offsets.pointCount, content.pointCount, true);
  header.setFloat32(offsets.samplesPerPoint, content.samplesPerPoint, true);
  header.setFloat32(offsets.startFraction, content.startFraction, true);
  header.setFloat32(offsets.peak, content.peak, true);
  header.setFloat32(offsets.rms, content.rms, true);
  header.setFloat32(offsets.frequency, content.frequency ?? 0, true);
  header.setFloat32(offsets.cursor, content.cursor ?? -1, true);
  header.setFloat32(offsets.cycleStart, content.cycleStart ?? 0, true);
  header.setFloat32(
    offsets.spanQuarterNotes,
    content.spanQuarterNotes ?? 0,
    true,
  );
  header.setFloat32(
    offsets.barQuarterNotes,
    content.barQuarterNotes ?? 0,
    true,
  );
  header.setFloat32(offsets.viewStart, content.viewStart ?? 0, true);
  header.setFloat32(offsets.viewEnd, content.viewEnd ?? 1, true);
  header.setFloat32(
    offsets.visibleSamples,
    content.visibleSamples ?? content.pointCount,
    true,
  );

  const data = new Float32Array(buffer, frameHeader.size, dataLength);
  let offset = 0;
  for (const view of ordered) {
    const values = content.views.get(view) ?? [];
    data.set(Array.from(values), offset);
    offset += values.length;
  }
  return buffer;
}
