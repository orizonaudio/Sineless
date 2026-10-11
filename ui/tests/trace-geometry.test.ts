import { describe, expect, test } from "vitest";

import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  cancellationBands,
  columnPosition,
  fullScale,
  lanes,
  pointPosition,
} from "../src/render/trace-geometry.ts";

function frame(
  mode: FrameMode,
  views: [ChannelView, number[]][],
  startFraction = 0,
): Frame {
  const first = views[0]?.[1] ?? [];
  return {
    sequence: 0,
    mode,
    triggered: true,
    frozen: false,
    roll: false,
    pointCount: mode === FrameMode.minMax ? first.length / 2 : first.length,
    samplesPerPoint: 1,
    startFraction,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    partial: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(
      views.map(([view, values]) => [view, new Float32Array(values)]),
    ),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
  };
}

const ys = (vertices: Float32Array) =>
  vertices.filter((_, index) => index % 2 === 1);
const xs = (vertices: Float32Array) =>
  vertices.filter((_, index) => index % 2 === 0);

describe("trace placement", () => {
  test("places columns and raw points by the part of the span a frame covers", () => {
    const zoomed = {
      ...frame(FrameMode.raw, [[ChannelView.left, [0, 0, 0, 0, 0]]]),
      viewStart: 0.25,
      viewEnd: 0.5,
      visibleSamples: 4,
      startFraction: 0.5,
    };
    expect(columnPosition(zoomed, 0)).toBeCloseTo(0.25);
    expect(columnPosition(zoomed, 5)).toBeCloseTo(0.5);
    expect(pointPosition(zoomed, 0.5)).toBeCloseTo(0.25);
    expect(pointPosition(zoomed, 4.5)).toBeCloseTo(0.5);
  });

  test("stacks views in equal lanes from the top, or overlays them", () => {
    expect(lanes(2, ViewLayout.stacked)).toEqual([
      { center: 0.5, halfHeight: 0.5 },
      { center: -0.5, halfHeight: 0.5 },
    ]);
    expect(lanes(2, ViewLayout.overlaid)).toEqual([
      { center: 0, halfHeight: 1 },
      { center: 0, halfHeight: 1 },
    ]);
  });

  test("maps the vertical scale in dBFS to full scale", () => {
    expect(fullScale(0)).toBe(1);
    expect(fullScale(-6)).toBeCloseTo(0.501, 3);
  });
});

describe("cancellation bands", () => {
  const layout = {
    widthPixels: 400,
    heightPixels: 300,
    verticalScaleDb: 0,
    layout: ViewLayout.stacked,
    lineWidthPixels: 1,
  };
  const compareFrame = (sum: number[]) => ({
    ...frame(FrameMode.minMax, [[ChannelView.left, [0, 0, 0, 0, 0, 0, 0, 0]]]),
    views: new Map(),
    lanes: [
      new Float32Array([-0.5, 0.5, -0.5, 0.5, -0.5, 0.5, -0.5, 0.5]),
      new Float32Array([-0.4, 0.4, -0.4, 0.4, -0.4, 0.4, -0.4, 0.4]),
      new Float32Array(sum),
    ],
    comparison: { sumGain: -3, fixedGain: 3, matches: [] },
  });
  const both = [true, true];

  test("shade where the sum falls 1 dB below what unrelated tracks would give", () => {
    // Unrelated, 0.5 and 0.4 sum to about 0.64: 0.6 is within 1 dB, 0.5 is 2 dB below it.
    const near = cancellationBands(
      compareFrame([-0.6, 0.6, -0.6, 0.6, -0.6, 0.6, -0.6, 0.6]),
      layout,
      both,
    );
    expect(near).toHaveLength(0);
    const below = cancellationBands(
      compareFrame([-0.5, 0.5, -0.5, 0.5, -0.5, 0.5, -0.5, 0.5]),
      layout,
      both,
    );
    expect(below).toHaveLength(12);
  });

  test("shade only the columns that cancel", () => {
    // Columns 1 and 2 cancel (0.1 against 0.5); 0 and 3 do not.
    const bands = cancellationBands(
      compareFrame([-0.9, 0.9, -0.1, 0.1, -0.1, 0.1, -0.9, 0.9]),
      layout,
      both,
    );
    expect(bands).toHaveLength(12); // One quad: the two adjacent columns merge.
    const x = xs(bands);
    expect(Math.min(...x)).toBeCloseTo(-0.5); // Column 1 starts a quarter across.
    expect(Math.max(...x)).toBeCloseTo(0.5);
    const y = ys(bands);
    expect(Math.max(...y)).toBeCloseTo(-1 / 3); // In the bottom of three lanes.
    expect(Math.min(...y)).toBeCloseTo(-1);
  });

  test("ignore quiet stretches and bridge short gaps", () => {
    const quietFrame = compareFrame([
      -0.1, 0.1, -0.1, 0.1, -0.1, 0.1, -0.1, 0.1,
    ]);
    const quiet = {
      ...quietFrame,
      lanes: [
        new Float32Array([-0.5, 0.5, -0.01, 0.01, -0.5, 0.5, -0.5, 0.5]),
        new Float32Array([-0.4, 0.4, -0.01, 0.01, -0.4, 0.4, -0.4, 0.4]),
        quietFrame.lanes[2] ?? new Float32Array(),
      ],
    };
    // Column 1 is quiet, but bridges columns 0 and 2 into one stretch with column 3.
    const bands = cancellationBands(quiet, layout, both);
    expect(bands).toHaveLength(12);
    expect(Math.min(...xs(bands))).toBeCloseTo(-1);
    expect(Math.max(...xs(bands))).toBeCloseTo(1);
  });

  test("measure against the compared tracks only", () => {
    // The sum (0.6) is well below a loud third track (0.9), which is not compared.
    const sum = new Float32Array([-0.6, 0.6, -0.6, 0.6, -0.6, 0.6, -0.6, 0.6]);
    const loud = new Float32Array([-0.9, 0.9, -0.9, 0.9, -0.9, 0.9, -0.9, 0.9]);
    const plain = compareFrame([]);
    const [first, second] = plain.lanes;
    const threeTracks = {
      ...plain,
      lanes: [first ?? sum, loud, second ?? sum, sum],
    };
    expect(
      cancellationBands(threeTracks, layout, [true, false, true]),
    ).toHaveLength(0);
    const bands = cancellationBands(threeTracks, layout, [true, true, true]);
    expect(bands).toHaveLength(12);
    expect(Math.min(...ys(bands))).toBeCloseTo(-1); // In the bottom of four lanes.
    expect(Math.max(...ys(bands))).toBeCloseTo(-0.5);
  });

  test("shade nothing outside the Compare view", () => {
    const plain = compareFrame([0, 0, 0, 0, 0, 0, 0, 0]);
    expect(
      cancellationBands({ ...plain, comparison: undefined }, layout, both),
    ).toHaveLength(0);
  });
});
