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
    partial: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    panReach: 0,
    invalidSamples: 0,
    invalidLanes: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 4,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: pointCount,
    views: new Map(),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
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

  test("finds crossings only where a trace with silent stretches crosses zero", () => {
    // Digital silence (exact zeros) before, between and after two crossings: one rising into
    // the positive part at 2.5, one falling out of it at 5.5.
    const values = new Float32Array([0, 0, -1, 1, 1, 1, -1, 0, 0, 0]);
    const crossings = zeroCrossings(frameOf(FrameMode.raw, 10), values);
    expect(crossings).toHaveLength(2);
    expect(crossings[0]).toBeCloseTo(2.5 / 10);
    expect(crossings[1]).toBeCloseTo(5.5 / 10);
    // One sample exactly on zero between the two sides: the crossing is that sample.
    const onZero = zeroCrossings(
      frameOf(FrameMode.raw, 3),
      new Float32Array([-1, 0, 1]),
    );
    expect(onZero).toHaveLength(1);
    expect(onZero[0]).toBeCloseTo(1 / 3);
    // Nothing anywhere in silence alone.
    expect(
      zeroCrossings(frameOf(FrameMode.raw, 6), new Float32Array(6)),
    ).toEqual([]);
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

  // Samples that are not finite (from a broken signal upstream) are passed over, as the engine
  // measures around them: no target at them, and the others as without them.
  describe.each([Number.NaN, Infinity, -Infinity])(
    "past a sample that is %s",
    (broken) => {
      test("finds the crossings of raw points and min/max columns", () => {
        const raw = new Float32Array([-1, broken, 1, 1, 3, -1, broken]);
        const crossings = zeroCrossings(frameOf(FrameMode.raw, 7), raw);
        expect(crossings).toHaveLength(1);
        expect(crossings[0]).toBeCloseTo(4.75 / 7);
        const columns = new Float32Array([
          broken,
          broken,
          -0.4,
          0.3,
          broken,
          0.5,
        ]);
        expect(zeroCrossings(frameOf(FrameMode.minMax, 3), columns)).toEqual([
          1.5 / 3,
        ]);
      });

      test("finds the extremes of raw points and min/max columns", () => {
        const raw = new Float32Array([broken, 0.1, 0.8, broken, -0.9, 0]);
        expect(extremePositions(frameOf(FrameMode.raw, 6), raw)).toEqual([
          2 / 6,
          4 / 6,
        ]);
        const columns = new Float32Array([
          broken,
          broken,
          -0.7,
          0.1,
          -0.1,
          0.9,
        ]);
        expect(extremePositions(frameOf(FrameMode.minMax, 3), columns)).toEqual(
          [2.5 / 3, 1.5 / 3],
        );
      });

      test("offers the finite extremes and zero as levels", () => {
        expect(
          levelTargets(new Float32Array([0.2, broken, -0.7, 0.4])),
        ).toEqual([expect.closeTo(0.4), expect.closeTo(-0.7), 0]);
      });

      test("offers nothing but zero when none is finite", () => {
        expect(
          extremePositions(
            frameOf(FrameMode.raw, 2),
            new Float32Array([broken, broken]),
          ),
        ).toEqual([]);
        expect(levelTargets(new Float32Array([broken]))).toEqual([0, 0, 0]);
      });
    },
  );

  test("snaps to the nearest target within the radius only", () => {
    expect(snap(0.52, [0.1, 0.5, 0.55], 0.05)).toBe(0.5);
    expect(snap(0.3, [0.1, 0.5], 0.05)).toBe(0.3);
  });
});
