// Property-based tests of the frame decoder against malformed input, and of encoding and decoding
// together, with frames generated from the protocol's enums and header field table.
import fc from "fast-check";
import { describe, expect, test } from "vitest";

import { ChannelView, FrameMode } from "../src/enums.ts";
import { decodeFrame, type Frame } from "../src/frame-decoder.ts";
import { encodeFrame, type FrameContent } from "../src/frame-encoder.ts";
import {
  FrameFlag,
  frameHeader,
  matchValues,
  type FieldType,
  type FrameHeaderFieldName,
} from "../src/frame.ts";

// The decoder's only failure: a buffer shorter than its header, or than the data it announces.
const documentedFailure =
  /^Frame (of \d+ bytes is shorter than its header|data is shorter than its header announces)$/;

// Decodes, and returns the frame, or the error if it is one the decoder documents. Anything else
// (a RangeError from a typed array, a TypeError) is thrown on and fails the property.
function decodeOrFail(buffer: ArrayBuffer): Frame | Error {
  try {
    return decodeFrame(buffer);
  } catch (error) {
    if (
      error instanceof Error &&
      error.constructor === Error &&
      documentedFailure.test(error.message)
    ) {
      return error;
    }
    throw error;
  }
}

const u32 = fc.integer({ min: 0, max: 0xffffffff });
// Any float32, NaN and infinities included: the engine sends NaN for "no value".
const f32 = fc.float();
const sample = fc.float();

const frameModes = Object.values(FrameMode);
const channelViews = Object.values(ChannelView);

// Small frames keep a run fast; the layout does not change with size.
const maxPoints = 16;
const maxLanes = 6;

const contentArbitrary: fc.Arbitrary<FrameContent> = fc
  .record({
    mode: fc.constantFrom(...frameModes),
    pointCount: fc.integer({ min: 0, max: maxPoints }),
    views: fc.subarray(channelViews),
    laneCount: fc.integer({ min: 0, max: maxLanes }),
    compared: fc.boolean(),
  })
  .chain(({ mode, pointCount, views, laneCount, compared }) => {
    const length = pointCount * (mode === FrameMode.minMax ? 2 : 1);
    const block = fc.array(sample, { minLength: length, maxLength: length });
    const matchCount = Math.max(laneCount - 1, 0);
    return fc.record({
      mode: fc.constant(mode),
      triggered: fc.boolean(),
      frozen: fc.boolean(),
      roll: fc.boolean(),
      synced: fc.boolean(),
      limited: fc.boolean(),
      partial: fc.boolean(),
      pointCount: fc.constant(pointCount),
      startFraction: f32,
      peak: f32,
      rms: f32,
      frequency: fc.option(f32, { nil: undefined }),
      cursor: fc.option(f32, { nil: undefined }),
      cycleStart: f32,
      cycleSeconds: f32,
      panReach: f32,
      invalidSamples: u32,
      invalidLanes: u32,
      spanQuarterNotes: f32,
      barQuarterNotes: f32,
      viewStart: f32,
      viewEnd: f32,
      visibleSamples: f32,
      views: fc
        .tuple(
          ...views.map((view) =>
            block.map((values) => [view, values] as const),
          ),
        )
        .map((entries) => new Map<ChannelView, number[]>(entries)),
      lanes: fc.array(block, { minLength: laneCount, maxLength: laneCount }),
      laneSet: u32,
      comparison: compared
        ? fc.record({
            sumGain: f32,
            fixedGain: f32,
            matches: fc.array(
              fc.record({
                correlation: f32,
                related: fc.boolean(),
                offset: f32,
                inverted: fc.boolean(),
              }),
              { minLength: matchCount, maxLength: matchCount },
            ),
          })
        : fc.constant(undefined),
    });
  })
  // A frame without a comparison has no `comparison` key at all.
  .map(({ comparison, ...rest }) =>
    comparison === undefined ? rest : { ...rest, comparison },
  );

// What each header field holds for a content, keyed by the field table's names: a field added to
// frame.ts without an entry here fails the typecheck.
function expectedHeader(
  content: FrameContent,
): Record<FrameHeaderFieldName, number> {
  const flags =
    (content.triggered ? FrameFlag.triggered : 0) |
    (content.frozen ? FrameFlag.frozen : 0) |
    (content.roll ? FrameFlag.roll : 0) |
    (content.synced === true ? FrameFlag.synced : 0) |
    (content.limited === true ? FrameFlag.limited : 0) |
    (content.partial === true ? FrameFlag.partial : 0) |
    (content.comparison === undefined ? 0 : FrameFlag.compared);
  const views = [...content.views.keys()].reduce<number>(
    (mask, view) => mask | (1 << view),
    0,
  );
  return {
    mode: content.mode,
    flags,
    views,
    laneCount: content.lanes?.length ?? 0,
    pointCount: content.pointCount,
    startFraction: content.startFraction,
    peak: content.peak,
    rms: content.rms,
    frequency: content.frequency ?? Number.NaN,
    cursor: content.cursor ?? -1,
    cycleStart: content.cycleStart ?? 0,
    spanQuarterNotes: content.spanQuarterNotes ?? 0,
    barQuarterNotes: content.barQuarterNotes ?? 0,
    viewStart: content.viewStart ?? 0,
    viewEnd: content.viewEnd ?? 1,
    visibleSamples: content.visibleSamples ?? content.pointCount,
    laneSet: content.laneSet ?? 0,
    sumGain: content.comparison?.sumGain ?? 0,
    fixedGain: content.comparison?.fixedGain ?? 0,
    cycleSeconds: content.cycleSeconds ?? 0,
    panReach: content.panReach ?? 0,
    invalidSamples: content.invalidSamples ?? 0,
    invalidLanes: content.invalidLanes ?? 0,
  };
}

const readField: Record<FieldType, (view: DataView, offset: number) => number> =
  {
    u8: (view, offset) => view.getUint8(offset),
    u16: (view, offset) => view.getUint16(offset, true),
    u32: (view, offset) => view.getUint32(offset, true),
    f32: (view, offset) => view.getFloat32(offset, true),
  };

function offsetOf(name: FrameHeaderFieldName): number {
  const field = frameHeader.fields.find((candidate) => candidate.name === name);
  if (field === undefined) {
    throw new Error(`No frame header field '${name}'`);
  }
  return field.offset;
}

const toFloat32 = (values: ArrayLike<number>): number[] =>
  Array.from(Float32Array.from(Array.from(values)));

// The frame the decoder should return for a content: float32 precision, "no frequency" (NaN)
// and "no cursor" (below 0) as undefined, and a polarity that is either related or not.
function expectedFrame(content: FrameContent): Frame {
  const header = expectedHeader(content);
  const f = Math.fround;
  const frequency = f(header.frequency);
  const cursor = f(header.cursor);
  return {
    mode: content.mode,
    triggered: content.triggered,
    frozen: content.frozen,
    roll: content.roll,
    pointCount: content.pointCount,
    startFraction: f(header.startFraction),
    peak: f(header.peak),
    rms: f(header.rms),
    frequency: Number.isNaN(frequency) ? undefined : frequency,
    synced: content.synced === true,
    limited: content.limited === true,
    partial: content.partial === true,
    cursor: cursor >= 0 ? cursor : undefined,
    cycleStart: f(header.cycleStart),
    cycleSeconds: f(header.cycleSeconds),
    panReach: f(header.panReach),
    invalidSamples: header.invalidSamples,
    invalidLanes: header.invalidLanes,
    spanQuarterNotes: f(header.spanQuarterNotes),
    barQuarterNotes: f(header.barQuarterNotes),
    viewStart: f(header.viewStart),
    viewEnd: f(header.viewEnd),
    visibleSamples: f(header.visibleSamples),
    views: new Map(
      channelViews
        .filter((view) => content.views.has(view))
        .map((view) => [
          view,
          new Float32Array(toFloat32(content.views.get(view) ?? [])),
        ]),
    ),
    lanes: (content.lanes ?? []).map(
      (lane) => new Float32Array(toFloat32(lane)),
    ),
    laneSet: header.laneSet,
    comparison:
      content.comparison === undefined
        ? undefined
        : {
            sumGain: f(content.comparison.sumGain),
            fixedGain: f(content.comparison.fixedGain),
            matches: content.comparison.matches.map((match) => ({
              correlation: f(match.correlation),
              related: match.related,
              offset: f(match.offset),
              inverted: match.related && match.inverted,
            })),
          },
  };
}

function dataLength(content: FrameContent): number {
  const perBlock =
    content.pointCount * (content.mode === FrameMode.minMax ? 2 : 1);
  const blocks = content.views.size + (content.lanes?.length ?? 0);
  const matches = (content.comparison?.matches.length ?? 0) * matchValues;
  return (blocks * perBlock + matches) * 4;
}

describe("frame encoding and decoding", () => {
  test("a frame decodes to what was encoded", () => {
    fc.assert(
      fc.property(contentArbitrary, (content) => {
        const buffer = encodeFrame(content);
        expect(buffer.byteLength).toBe(frameHeader.size + dataLength(content));
        expect(decodeFrame(buffer)).toEqual(expectedFrame(content));
      }),
    );
  });

  test("every header field holds its value at its offset in the field table", () => {
    fc.assert(
      fc.property(contentArbitrary, (content) => {
        const view = new DataView(encodeFrame(content), 0, frameHeader.size);
        const expected = expectedHeader(content);
        for (const field of frameHeader.fields) {
          const name = field.name as FrameHeaderFieldName;
          const value = expected[name];
          expect(readField[field.type](view, field.offset), name).toBe(
            field.type === "f32" ? Math.fround(value) : value,
          );
        }
      }),
    );
  });
});

describe("frame decoder against malformed input", () => {
  test("any bytes decode or fail with the documented error", () => {
    fc.assert(
      fc.property(
        fc.uint8Array({ maxLength: 4 * frameHeader.size }),
        (bytes) => {
          decodeOrFail(bytes.slice().buffer);
        },
      ),
    );
  });

  test("any header announcing few points, with any data, decodes or fails with the documented error", () => {
    // Random point counts are mostly huge and fail at once; small ones reach the views, lanes
    // and matches with whatever flags, view mask and lane count the header holds.
    fc.assert(
      fc.property(
        fc.uint8Array({
          minLength: frameHeader.size,
          maxLength: frameHeader.size,
        }),
        fc.integer({ min: 0, max: maxPoints }),
        fc.uint8Array({ maxLength: 2048 }),
        (header, pointCount, data) => {
          const bytes = new Uint8Array(header.length + data.length);
          bytes.set(header);
          bytes.set(data, header.length);
          new DataView(bytes.buffer).setUint32(
            offsetOf("pointCount"),
            pointCount,
            true,
          );
          decodeOrFail(bytes.buffer);
        },
      ),
    );
  });

  test("a truncated frame fails with the documented error", () => {
    fc.assert(
      fc.property(
        contentArbitrary,
        fc.double({ min: 0, max: 1, maxExcluded: true, noNaN: true }),
        (content, fraction) => {
          const buffer = encodeFrame(content);
          const length = Math.floor(fraction * buffer.byteLength);
          expect(decodeOrFail(buffer.slice(0, length))).toBeInstanceOf(Error);
        },
      ),
    );
  });

  test("a frame with corrupted bytes decodes or fails with the documented error", () => {
    fc.assert(
      fc.property(
        contentArbitrary,
        fc.array(fc.tuple(fc.nat(), fc.integer({ min: 0, max: 255 })), {
          minLength: 1,
          maxLength: 8,
        }),
        (content, changes) => {
          const bytes = new Uint8Array(encodeFrame(content));
          for (const [position, value] of changes) {
            bytes[position % bytes.length] = value;
          }
          const decoded = decodeOrFail(bytes.buffer);
          if (!(decoded instanceof Error)) {
            // Whatever the header claims, each view and lane holds what its point count and
            // mode say: one value per point, or a min/max pair.
            const perTrace =
              decoded.pointCount * (decoded.mode === FrameMode.minMax ? 2 : 1);
            for (const values of [
              ...decoded.views.values(),
              ...decoded.lanes,
            ]) {
              expect(values.length).toBe(perTrace);
            }
          }
        },
      ),
    );
  });

  test("bytes after the frame are ignored", () => {
    fc.assert(
      fc.property(
        contentArbitrary,
        fc.uint8Array({ minLength: 1, maxLength: 64 }),
        (content, extra) => {
          const frame = new Uint8Array(encodeFrame(content));
          const bytes = new Uint8Array(frame.length + extra.length);
          bytes.set(frame);
          bytes.set(extra, frame.length);
          expect(decodeFrame(bytes.buffer)).toEqual(expectedFrame(content));
        },
      ),
    );
  });
});
