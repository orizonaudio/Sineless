import { expect, test } from "vitest";

import { ChannelView, FrameMode } from "../src/enums.ts";
import { decodeFrame } from "../src/frame-decoder.ts";
import { encodeFrame } from "../src/frame-encoder.ts";

import golden from "./data/golden-frame.json" with { type: "json" };

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
    frozen: false,
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
    views: new Map([
      [ChannelView.mid, golden.views.mid],
      [ChannelView.left, golden.views.left],
    ]),
  });
  expect(toHex(buffer)).toBe(golden.hex);
  expect(decodeFrame(buffer).pointCount).toBe(golden.header.pointCount);
});
