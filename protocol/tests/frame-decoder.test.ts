import { describe, expect, test } from "vitest";

import { ChannelView, FrameMode } from "../src/enums.ts";
import { decodeFrame } from "../src/frame-decoder.ts";

import golden from "./data/golden-frame.json" with { type: "json" };
import { goldenLanes } from "./golden.ts";

function fromHex(hex: string): ArrayBuffer {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return bytes.buffer;
}

// The same golden frame pins the C++ encoder (engine FrameEncoderTests).
describe("frame decoder", () => {
  test("decodes the golden frame", () => {
    const frame = decodeFrame(fromHex(golden.hex));
    expect(frame.sequence).toBe(golden.header.sequence);
    expect(frame.mode).toBe(FrameMode.minMax);
    expect(frame.triggered).toBe(true);
    expect(frame.frozen).toBe(true);
    expect(frame.panReach).toBe(1.5);
    expect(frame.roll).toBe(false);
    expect(frame.pointCount).toBe(golden.header.pointCount);
    expect(frame.samplesPerPoint).toBe(golden.header.samplesPerPoint);
    expect(frame.startFraction).toBe(golden.header.startFraction);
    expect(frame.peak).toBe(golden.header.peak);
    expect(frame.rms).toBe(golden.header.rms);
    expect(frame.frequency).toBe(golden.header.frequency);
    expect(frame.cursor).toBeUndefined();
    expect(frame.synced).toBe(false);
    expect(frame.limited).toBe(false);
    expect(frame.partial).toBe(false);
    expect([...frame.views.keys()]).toEqual([
      ChannelView.left,
      ChannelView.mid,
    ]);
    expect(Array.from(frame.views.get(ChannelView.left) ?? [])).toEqual(
      golden.views.left,
    );
    expect(Array.from(frame.views.get(ChannelView.mid) ?? [])).toEqual(
      golden.views.mid,
    );
  });

  test("decodes the golden lanes frame", () => {
    const frame = decodeFrame(fromHex(goldenLanes.hex));
    expect(frame.views.size).toBe(0);
    expect(frame.laneSet).toBe(goldenLanes.header.laneSet);
    expect(frame.synced).toBe(true);
    expect(frame.cursor).toBe(goldenLanes.header.cursor);
    expect(frame.cycleSeconds).toBe(goldenLanes.header.cycleSeconds);
    expect(frame.frequency).toBeUndefined();
    expect(frame.lanes.map((lane) => Array.from(lane))).toEqual(
      goldenLanes.lanes,
    );
    expect(frame.comparison).toEqual({
      sumGain: goldenLanes.header.sumGain,
      fixedGain: goldenLanes.header.fixedGain,
      matches: goldenLanes.matches,
    });
  });

  test("has no lanes in a single-track frame", () => {
    const frame = decodeFrame(fromHex(golden.hex));
    expect(frame.lanes).toEqual([]);
    expect(frame.laneSet).toBe(0);
    expect(frame.comparison).toBeUndefined();
  });

  test("reports an unavailable frequency (NaN) as undefined", () => {
    const buffer = fromHex(golden.hex);
    new DataView(buffer).setFloat32(28, Number.NaN, true);
    expect(decodeFrame(buffer).frequency).toBeUndefined();
  });

  test("rejects truncated frames", () => {
    const buffer = fromHex(golden.hex);
    expect(() => decodeFrame(buffer.slice(0, 16))).toThrow(
      /shorter than its header/,
    );
    expect(() => decodeFrame(buffer.slice(0, 80))).toThrow(
      /shorter than its header announces/,
    );
    const lanes = fromHex(goldenLanes.hex);
    expect(() => decodeFrame(lanes.slice(0, lanes.byteLength - 4))).toThrow(
      /shorter than its header announces/,
    );
  });
});
