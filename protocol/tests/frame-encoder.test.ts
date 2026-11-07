import { expect, test } from "vitest";

import { ChannelView, FrameMode } from "../src/enums.ts";
import { decodeFrame } from "../src/frame-decoder.ts";
import { encodeFrame } from "../src/frame-encoder.ts";
import golden from "./data/golden-frame.json" with { type: "json" };
import { goldenLanes } from "./golden.ts";

function toHex(buffer: ArrayBuffer): string {
  return Array.from(new Uint8Array(buffer), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
}

test("encodes the golden frame byte for byte", () => {
  const buffer = encodeFrame({
    mode: FrameMode.minMax,
    triggered: true,
    frozen: true,
    freeRunning: false,
    pointCount: golden.header.pointCount,
    startFraction: golden.header.startFraction,
    peak: golden.header.peak,
    rms: golden.header.rms,
    viewStart: golden.header.viewStart,
    viewEnd: golden.header.viewEnd,
    visibleSamples: golden.header.visibleSamples,
    panReach: golden.header.panReach,
    invalidSamples: golden.header.invalidSamples,
    invalidLanes: golden.header.invalidLanes,
    sampleRate: golden.header.sampleRate,
    spanSeconds: golden.header.spanSeconds,
    readoutView: golden.header.readoutView as ChannelView,
    triggerView: golden.header.triggerView as ChannelView,
    triggerPosition: golden.header.triggerPosition,
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
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    freeRunning: false,
    synced: true,
    pointCount: header.pointCount,
    startFraction: header.startFraction,
    peak: header.peak,
    rms: header.rms,
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
    sampleRate: header.sampleRate,
    comparison: {
      sumGain: header.sumGain,
      fixedGain: header.fixedGain,
      matches: goldenLanes.matches,
    },
  });
  expect(toHex(buffer)).toBe(goldenLanes.hex);
});
