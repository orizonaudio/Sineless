import { describe, expect, test } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  extremePositions,
  levelTargets,
  snap,
  zeroCrossings,
} from "../src/view/snap.ts";

// A frame of the whole span with the given points; other fields do not matter here.
function frameOf(mode: FrameMode, pointCount: number): Frame {
  return {
    sequence: 0,
    mode,
    triggered: false,
    frozen: false,
    roll: false,
    pointCount,
    samplesPerPoint: 1,
    startFraction: 0,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    cursor: undefined,
    cycleStart: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 4,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: pointCount,
    views: new Map(),
  };
}

describe("cursor snapping", () => {
  test("finds interpolated zero crossings of raw points, both ways", () => {
    const values = new Float32Array([-1, 1, 1, 3, -1]);
    const crossings = zeroCrossings(frameOf(FrameMode.raw, 5), values);
    expect(crossings).toHaveLength(2);
    expect(crossings[0]).toBeCloseTo(0.5 / 5); // Halfway from -1 to 1.
    expect(crossings[1]).toBeCloseTo(3.75 / 5); // Three quarters from 3 to -1.
  });

  test("takes min/max columns that span zero as crossings at their centre", () => {
    const values = new Float32Array([0.1, 0.5, -0.4, 0.3, -0.6, -0.2, -0.1, 0]);
    expect(zeroCrossings(frameOf(FrameMode.minMax, 4), values)).toEqual([
      1.5 / 4,
      3.5 / 4,
    ]);
  });

  test("finds where raw points reach their highest and lowest", () => {
    const values = new Float32Array([0.1, 0.8, 0.2, -0.5, -0.9, 0]);
    expect(extremePositions(frameOf(FrameMode.raw, 6), values)).toEqual([
      1 / 6,
      4 / 6,
    ]);
  });

  test("finds the min/max columns holding the extremes, at their centre", () => {
    const values = new Float32Array([-0.2, 0.3, -0.7, 0.1, -0.1, 0.9, 0, 0.2]);
    expect(extremePositions(frameOf(FrameMode.minMax, 4), values)).toEqual([
      2.5 / 4,
      1.5 / 4,
    ]);
  });

  test("offers the extremes and zero as levels", () => {
    expect(levelTargets(new Float32Array([0.2, -0.7, 0.4]))).toEqual([
      expect.closeTo(0.4),
      expect.closeTo(-0.7),
      0,
    ]);
  });

  test("snaps to the nearest target within the radius only", () => {
    expect(snap(0.52, [0.1, 0.5, 0.55], 0.05)).toBe(0.5);
    expect(snap(0.3, [0.1, 0.5], 0.05)).toBe(0.3);
  });
});
