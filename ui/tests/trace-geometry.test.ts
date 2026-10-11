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
    panReach: 0,
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
  // Each lane's match against this track: this track's own is zeros, never related.
  const own = { correlation: 0, related: false, offset: 0, inverted: false };
  const alike = { correlation: 0.9, related: true, offset: 0, inverted: false };
  const relatedMatches = [own, alike, alike];
  const unrelatedMatches = [own, own, own];
  const compareFrame = (sum: number[]) => ({
    ...frame(FrameMode.minMax, [[ChannelView.left, [0, 0, 0, 0, 0, 0, 0, 0]]]),
    views: new Map(),
    lanes: [
      new Float32Array([-0.5, 0.5, -0.5, 0.5, -0.5, 0.5, -0.5, 0.5]),
      new Float32Array([-0.4, 0.4, -0.4, 0.4, -0.4, 0.4, -0.4, 0.4]),
      new Float32Array(sum),
    ],
    comparison: { sumGain: -3, fixedGain: 3, matches: relatedMatches },
  });
  const both = [true, true];

  test("leave tracks that beat unshaded unless they are related", () => {
    // Two steady tones a little apart in pitch: their sum swells and dips at the difference.
    const columns = 1000;
    const column = (signal: (t: number) => number) => {
      const values: number[] = [];
      for (let c = 0; c < columns; c++) {
        let low = 0;
        let high = 0;
        for (let i = 0; i < 48; i++) {
          const value = signal((c * 48 + i) / 48000);
          low = Math.min(low, value);
          high = Math.max(high, value);
        }
        values.push(low, high);
      }
      return new Float32Array(values);
    };
    const a = (t: number) => 0.5 * Math.sin(2 * Math.PI * 220 * t);
    const b = (t: number) => 0.5 * Math.sin(2 * Math.PI * 223 * t);
    const beating = {
      ...frame(FrameMode.minMax, [[ChannelView.left, [0, 0]]]),
      views: new Map(),
      pointCount: columns,
      lanes: [column(a), column(b), column((t) => a(t) + b(t))],
      comparison: { sumGain: 0, fixedGain: 0, matches: unrelatedMatches },
    };
    expect(cancellationBands(beating, layout, both)).toHaveLength(0);
    const related = {
      ...beating,
      comparison: { ...beating.comparison, matches: relatedMatches },
    };
    expect(cancellationBands(related, layout, both).length).toBeGreaterThan(0);
  });

  test("leave unrelated noise unshaded", () => {
    let seed = 11;
    const noise = () => {
      seed = (seed * 1103515245 + 12345) % 2147483648;
      return seed / 2147483648 - 0.5;
    };
    const columns = 2000;
    const a: number[] = [];
    const b: number[] = [];
    const sum: number[] = [];
    for (let column = 0; column < columns; column++) {
      let lowA = 0,
        highA = 0,
        lowB = 0,
        highB = 0,
        lowS = 0,
        highS = 0;
      for (let i = 0; i < 20; i++) {
        const x = noise();
        const y = noise();
        lowA = Math.min(lowA, x);
        highA = Math.max(highA, x);
        lowB = Math.min(lowB, y);
        highB = Math.max(highB, y);
        lowS = Math.min(lowS, x + y);
        highS = Math.max(highS, x + y);
      }
      a.push(lowA, highA);
      b.push(lowB, highB);
      sum.push(lowS, highS);
    }
    const unrelated = {
      ...frame(FrameMode.minMax, [[ChannelView.left, a]]),
      views: new Map(),
      lanes: [new Float32Array(a), new Float32Array(b), new Float32Array(sum)],
      comparison: { sumGain: 0, fixedGain: 0, matches: relatedMatches },
    };
    const shaded = xs(cancellationBands(unrelated, layout, both));
    // Quads span columns: the share of the lane's width they cover.
    let covered = 0;
    for (let quad = 0; quad < shaded.length; quad += 6) {
      covered += Math.abs((shaded[quad + 1] ?? 0) - (shaded[quad] ?? 0)) / 2;
    }
    expect(covered).toBeLessThan(0.02);
  });

  test("shade where the sum falls 3 dB below the louder track", () => {
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
