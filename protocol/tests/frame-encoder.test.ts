import { expect, test } from "vitest";

import { ChannelView, FrameMode } from "../src/enums.ts";
import { decodeFrame } from "../src/frame-decoder.ts";
import { encodeFrame } from "../src/frame-encoder.ts";
import { frameHeader } from "../src/frame.ts";
import golden from "./data/golden-frame.json" with { type: "json" };
import { goldenLanes } from "./golden.ts";

function toHex(buffer: ArrayBuffer): string {
  return Array.from(new Uint8Array(buffer), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
}

test("encodes the golden frame byte for byte", () => {
  const buffer = encodeFrame({
    sequence: golden.header.sequence,
    mode: FrameMode.minMax,
    triggered: true,
    frozen: true,
    roll: false,
    pointCount: golden.header.pointCount,
    samplesPerPoint: golden.header.samplesPerPoint,
    startFraction: golden.header.startFraction,
    peak: golden.header.peak,
    rms: golden.header.rms,
    frequency: golden.header.frequency,
    viewStart: golden.header.viewStart,
    viewEnd: golden.header.viewEnd,
    visibleSamples: golden.header.visibleSamples,
    panReach: golden.header.panReach,
    invalidSamples: golden.header.invalidSamples,
    invalidLanes: golden.header.invalidLanes,
    views: new Map([
      [ChannelView.mid, golden.views.mid],
      [ChannelView.left, golden.views.left],
    ]),
  });
  expect(toHex(buffer)).toBe(golden.hex);
  expect(decodeFrame(buffer).pointCount).toBe(golden.header.pointCount);
});

test("encodes the golden lanes frame byte for byte", () => {
  const { header } = goldenLanes;
  const buffer = encodeFrame({
    sequence: header.sequence,
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    roll: false,
    synced: true,
    pointCount: header.pointCount,
    samplesPerPoint: header.samplesPerPoint,
    startFraction: header.startFraction,
    peak: header.peak,
    rms: header.rms,
    frequency: header.frequency,
    cursor: header.cursor,
    cycleStart: header.cycleStart,
    spanQuarterNotes: header.spanQuarterNotes,
    barQuarterNotes: header.barQuarterNotes,
    cycleSeconds: header.cycleSeconds,
    visibleSamples: header.visibleSamples,
    views: new Map(),
    lanes: goldenLanes.lanes,
    laneSet: header.laneSet,
    invalidSamples: header.invalidSamples,
    invalidLanes: header.invalidLanes,
    comparison: {
      sumGain: header.sumGain,
      fixedGain: header.fixedGain,
      matches: goldenLanes.matches,
    },
  });
  expect(toHex(buffer)).toBe(goldenLanes.hex);
});

test("encodes a frequency not found as NaN, the protocol's no value", () => {
  const { header } = goldenLanes;
  const buffer = encodeFrame({
    sequence: 1,
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    roll: false,
    synced: false,
    pointCount: 0,
    samplesPerPoint: 1,
    startFraction: 0,
    peak: header.peak,
    rms: header.rms,
    frequency: undefined,
    cycleStart: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    cycleSeconds: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(),
    lanes: [],
    laneSet: 0,
  });
  const offset = frameHeader.fields.find(
    (field) => field.name === "frequency",
  )?.offset;
  expect(offset).toBeDefined();
  expect(new DataView(buffer).getFloat32(offset ?? 0, true)).toBeNaN();
});
