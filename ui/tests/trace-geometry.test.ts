import { describe, expect, test } from "vitest";

import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  cancellationReachDivisor,
  maxLanes,
} from "../../protocol/src/limits.ts";
import {
  cancellationBands,
  cancellationLevels,
  columnPosition,
  coverageAttachments,
  coverageSlot,
  coverageTargets,
  fullScale,
  lanes,
  playheadEdges,
  pointPosition,
  tracesPerPass,
} from "../src/render/trace-geometry.ts";

function frame(
  mode: FrameMode,
  views: [ChannelView, number[]][],
  startFraction = 0,
): Frame {
  const first = views[0]?.[1] ?? [];
  return {
    mode,
    triggered: true,
    frozen: false,
    freeRunning: false,
    pointCount: mode === FrameMode.minMax ? first.length / 2 : first.length,
    startFraction,
    peak: 0,
    rms: 0,
    synced: false,
    limited: false,
    partial: false,
    noTempo: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    panReach: 0,
    invalidSamples: 0,
    invalidLanes: 0,
    sampleRate: 48000,
    spanSeconds: 0,
    readoutView: ChannelView.mid,
    triggerView: ChannelView.mid,
    triggerPosition: undefined,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    pointsPerSample: 1,
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

  test("places a reconstructed frame's samples on their samples, the wave between them", () => {
    // Eight points per sample: point 8k is sample k, point 8k + 4 halfway to the next.
    const deep = {
      ...frame(FrameMode.raw, [
        [ChannelView.left, new Array<number>(33).fill(0)],
      ]),
      viewStart: 0.25,
      viewEnd: 0.5,
      visibleSamples: 4,
      startFraction: 0.5,
      pointsPerSample: 8,
    };
    const sample = (k: number) => 0.25 + ((k - 0.5) / 4) * 0.25;
    for (let k = 0; k <= 4; k++) {
      expect(pointPosition(deep, 8 * k)).toBeCloseTo(sample(k), 12);
    }
    expect(pointPosition(deep, 12)).toBeCloseTo(sample(1.5), 12);
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

// Display scales in use: macOS (1, 2), Windows (1.25, 1.5, 1.75, 3) and browser zoom.
describe.each([1, 1.25, 1.5, 1.75, 2, 3])("playhead at %sx", (pixelRatio) => {
  const widthPixels = Math.round(737 * pixelRatio);
  const positions = Array.from({ length: 2001 }, (_, step) => step / 2000);
  const placed = positions.map((position) => {
    const { left, right } = playheadEdges(position, widthPixels, pixelRatio);
    return {
      position,
      left: ((left + 1) / 2) * widthPixels,
      right: ((right + 1) / 2) * widthPixels,
    };
  });

  // A line straddling pixels is antialiased into a dimmer, wider one, by a different amount at
  // each subpixel position: as it sweeps, it flickers.
  test("covers whole device pixels at every position, so it does not flicker as it moves", () => {
    for (const { left, right } of placed) {
      expect(left).toBeCloseTo(Math.round(left), 6);
      expect(right).toBeCloseTo(Math.round(right), 6);
    }
  });

  // A CSS pixel in whole device pixels, as a 1px border is: thinner on a 2x screen would be
  // half as bright.
  test("keeps one width: a CSS pixel snapped down to whole device pixels, at least one", () => {
    const widths = new Set(
      placed.map(({ left, right }) => (right - left).toFixed(6)),
    );
    expect(widths.size).toBe(1);
    const width = Number([...widths][0]);
    expect(width).toBeGreaterThanOrEqual(1);
    expect(width).toBeLessThanOrEqual(Math.max(1, pixelRatio));
    expect(width).toBeGreaterThan(pixelRatio - 1);
  });

  // Zoomed or panned away, the playhead can be left of the screen: the dimmed previous cycle,
  // which starts at its left edge, then covers the whole screen.
  test.each([-1e-4, -0.5, -3])(
    "is left of the screen at position %s",
    (position) => {
      const { left } = playheadEdges(position, widthPixels, pixelRatio);
      expect(left).toBeLessThanOrEqual(-1);
    },
  );

  // At the screen's edges too: there the screen clips it, as it always has.
  test("stays centred on its position to the nearest device pixel", () => {
    let previous = -Infinity;
    for (const { position, left, right } of placed) {
      expect(
        Math.abs((left + right) / 2 - position * widthPixels),
      ).toBeLessThanOrEqual(0.5 + 1e-6);
      expect(left).toBeGreaterThanOrEqual(previous - 1e-6);
      previous = left;
    }
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

  // A Compare frame of min/max columns with these peak levels per column, for the two tracks and
  // their sum.
  const columnsFrame = (a: number[], b: number[], sum: number[]) => {
    const columns = (levels: number[]) =>
      new Float32Array(levels.flatMap((level) => [-level, level]));
    return {
      ...frame(FrameMode.minMax, [[ChannelView.left, a.flatMap(() => [0, 0])]]),
      views: new Map(),
      lanes: [columns(a), columns(b), columns(sum)],
      comparison: { sumGain: -3, fixedGain: 3, matches: relatedMatches },
    };
  };
  const quads = (bands: Float32Array) => bands.length / 12;

  test("shade nothing with only one track compared", () => {
    // The tracks cancel, and the other track is related, but it alone is in the sum: nothing to
    // cancel against.
    const cancelling = compareFrame([
      -0.1, 0.1, -0.1, 0.1, -0.1, 0.1, -0.1, 0.1,
    ]);
    expect(cancellationBands(cancelling, layout, [false, true])).toHaveLength(
      0,
    );
  });

  test("shade two cancelling stretches far apart as two bands", () => {
    // Columns 0-1 and 10-11 cancel; the eight between do not, far more than a short gap.
    const loud = new Array<number>(12).fill(0.5);
    const sum = loud.map((_, column) =>
      column < 2 || column >= 10 ? 0.05 : 0.9,
    );
    const bands = cancellationBands(
      columnsFrame(loud, loud, sum),
      layout,
      both,
    );
    expect(quads(bands)).toBe(2);
    // One stretch at the start, then none to the end: the band stops where cancelling does.
    const first = sum.map((level, column) => (column < 2 ? level : 0.9));
    const alone = cancellationBands(
      columnsFrame(loud, loud, first),
      layout,
      both,
    );
    expect(quads(alone)).toBe(1);
    expect(Math.max(...xs(alone))).toBeCloseTo(-1 + (2 * 2) / 12);
  });

  test("measure a column by its larger excursion, either side of zero", () => {
    // The tracks swing to −0.5 but only to +0.1; the sum's ±0.08 is well under the larger.
    const asymmetric = new Float32Array([-0.5, 0.1, -0.5, 0.1]);
    const cancelling = {
      ...columnsFrame([0, 0], [0, 0], [0, 0]),
      lanes: [
        asymmetric,
        asymmetric,
        new Float32Array([-0.08, 0.08, -0.08, 0.08]),
      ],
    };
    expect(quads(cancellationBands(cancelling, layout, both))).toBe(1);
    // And the other way up: to +0.5, but only to −0.1.
    const upwards = new Float32Array([-0.1, 0.5, -0.1, 0.5]);
    expect(
      quads(
        cancellationBands(
          {
            ...cancelling,
            lanes: [upwards, upwards, cancelling.lanes[2] ?? upwards],
          },
          layout,
          both,
        ),
      ),
    ).toBe(1);
  });

  test("shade no quiet stretch at the start or end, whatever its sum", () => {
    // Columns 0-1 and 6-7 are some 46 dB below the loud middle, and their sum lower still.
    const tracks = [0.0025, 0.0025, 0.5, 0.5, 0.5, 0.5, 0.0025, 0.0025];
    const sum = [0.0001, 0.0001, 0.9, 0.9, 0.9, 0.9, 0.0001, 0.0001];
    expect(
      cancellationBands(columnsFrame(tracks, tracks, sum), layout, both),
    ).toHaveLength(0);
  });

  test("shade the part of the span on screen, zoomed in", () => {
    // Columns 1 and 2 cancel: the middle half of the span, which the view zooms to.
    const bands = cancellationBands(
      compareFrame([-0.9, 0.9, -0.1, 0.1, -0.1, 0.1, -0.9, 0.9]),
      { ...layout, view: { start: 0.25, end: 0.75 } },
      both,
    );
    expect(quads(bands)).toBe(1);
    expect(Math.min(...xs(bands))).toBeCloseTo(-1);
    expect(Math.max(...xs(bands))).toBeCloseTo(1);
  });

  test("shade raw points, zoomed in past the columns", () => {
    // Eight raw points of each track and of a sum that cancels throughout, the first point half
    // a sample after the span starts.
    const alternate = (level: number) =>
      new Float32Array(
        Array.from({ length: 8 }, (_, i) => (i % 2 ? -level : level)),
      );
    const raw = {
      ...frame(
        FrameMode.raw,
        [[ChannelView.left, new Array<number>(8).fill(0)]],
        0.5,
      ),
      views: new Map(),
      lanes: [alternate(0.5), alternate(-0.4), alternate(0.1)],
      comparison: { sumGain: -3, fixedGain: 3, matches: relatedMatches },
    };
    const bands = cancellationBands(raw, layout, both);
    expect(quads(bands)).toBe(1);
    expect(Math.min(...xs(bands))).toBeCloseTo(-1 + 2 * pointPosition(raw, 0));
    expect(Math.max(...xs(bands))).toBeCloseTo(-1 + 2 * pointPosition(raw, 8));
  });

  test.each([1, 8, 16])(
    "bridge a gap of a sample in a cancelling stretch at %i points per sample",
    (pointsPerSample) => {
      // Forty samples of two tracks and a sum that cancels but for one sample, each sample's
      // value held across the points drawn between samples.
      const samples = 40;
      const points = (samples - 1) * pointsPerSample + 1;
      const held = (value: (sample: number) => number) =>
        new Float32Array(
          Array.from({ length: points }, (_, i) =>
            value(Math.floor(i / pointsPerSample)),
          ),
        );
      const sign = (k: number) => (k % 2 ? -1 : 1);
      const raw = {
        ...frame(FrameMode.raw, [
          [ChannelView.left, new Array<number>(points).fill(0)],
        ]),
        visibleSamples: samples - 1,
        pointsPerSample,
        views: new Map(),
        lanes: [
          held((k) => 0.5 * sign(k)),
          held((k) => -0.4 * sign(k)),
          held((k) => (k === 20 ? 0.9 : 0.1) * sign(k)),
        ],
        comparison: { sumGain: -3, fixedGain: 3, matches: relatedMatches },
      };
      expect(quads(cancellationBands(raw, layout, both))).toBe(1);
    },
  );

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

  // A sample that is not finite (from a broken signal upstream), in a track and so in the sum:
  // measured around, as the engine measures the gains, so the cancelling stretch still shows.
  test.each([Number.NaN, Infinity, -Infinity])(
    "shade around a column that is %s",
    (broken) => {
      const cancelling = compareFrame([
        -0.1,
        0.1,
        broken,
        broken,
        -0.1,
        0.1,
        -0.1,
        0.1,
      ]);
      const [first, second, sum] = cancelling.lanes;
      const spiked = new Float32Array(first ?? []);
      spiked[3] = broken;
      const bands = cancellationBands(
        {
          ...cancelling,
          lanes: [
            spiked,
            second ?? new Float32Array(),
            sum ?? new Float32Array(),
          ],
        },
        layout,
        both,
      );
      expect(bands).toHaveLength(12); // One quad across the lane.
      expect(Math.min(...xs(bands))).toBeCloseTo(-1);
      expect(Math.max(...xs(bands))).toBeCloseTo(1);
    },
  );

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

// Each trace's coverage has a channel of its own until it is layered over the screen: two traces
// sharing one would draw as one, and a trace with none would vanish.
describe("the coverage channels of a frame's traces", () => {
  test("the most lanes a frame holds fit one pass", () => {
    expect(maxLanes).toBeLessThanOrEqual(tracesPerPass);
  });

  test.each(Array.from({ length: maxLanes }, (_, k) => k + 1))(
    "%i traces each get their own channel, in the targets cleared and read",
    (traces) => {
      const slots = Array.from({ length: traces }, (_, k) => coverageSlot(k));
      const used = new Set(
        slots.map(({ attachment, channel }) => attachment * 4 + channel),
      );
      expect(used.size).toBe(traces);
      for (const { channel } of slots) {
        expect(channel).toBeGreaterThanOrEqual(0);
        expect(channel).toBeLessThan(4);
      }
      // No target is cleared and read that no trace uses, and none used is left out.
      expect(coverageTargets(traces)).toBe(
        Math.max(...slots.map(({ attachment }) => attachment)) + 1,
      );
      expect(coverageTargets(traces)).toBeLessThanOrEqual(coverageAttachments);
    },
  );
});

// The levels Compare's shading compares, as a loop over each point's window computes them: the
// largest finite level within the reach either side, NaN where the point is not finite.
function levelsByLoop(frame: Frame, values: Float32Array): Float32Array {
  const minMax = frame.mode === FrameMode.minMax;
  const own = new Float32Array(frame.pointCount);
  for (let point = 0; point < frame.pointCount; point++) {
    own[point] = minMax
      ? Math.max(
          Math.abs(values[point * 2] ?? 0),
          Math.abs(values[point * 2 + 1] ?? 0),
        )
      : Math.abs(values[point] ?? 0);
  }
  const reach = minMax
    ? 0
    : Math.max(1, Math.floor(frame.pointCount / cancellationReachDivisor));
  const out = new Float32Array(own.length);
  for (let point = 0; point < own.length; point++) {
    let peak = 0;
    const last = Math.min(own.length - 1, point + reach);
    for (let near = Math.max(0, point - reach); near <= last; near++) {
      const level = own[near] ?? 0;
      peak = Number.isFinite(level) ? Math.max(peak, level) : peak;
    }
    out[point] = Number.isFinite(own[point]) ? peak : Number.NaN;
  }
  return out;
}

// Compare's shading takes each raw point's level in one pass (its cost follows the points, which a
// raw frame at about a sample a column has four times more of), the same as the loop over each
// point's window, as the native editor does (TraceGeometryTests): at the point counts where the
// reach changes, for min/max columns, a trace not finite throughout, runs of equal levels, a level
// not finite leaving the window, rising and falling levels, and random levels.
describe("cancellation levels", () => {
  const divisor = cancellationReachDivisor;
  let state = 12345;
  const random = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return (state >>> 8) / (1 << 24);
  };
  const traces: [string, Float32Array][] = [];
  for (const count of [
    1,
    2,
    divisor - 1,
    divisor,
    divisor + 1,
    2 * divisor - 1,
    2 * divisor,
    2 * divisor + 1,
    5 * divisor + 3,
    12000,
  ]) {
    traces.push([
      `random ${String(count)}`,
      Float32Array.from({ length: count }, () => {
        const draw = random();
        return draw < 0.05
          ? Number.NaN
          : draw < 0.07
            ? Number.POSITIVE_INFINITY
            : 2 * random() - 1;
      }),
    ]);
    traces.push([
      `rising ${String(count)}`,
      Float32Array.from({ length: count }, (_, point) => point),
    ]);
    traces.push([
      `falling ${String(count)}`,
      Float32Array.from({ length: count }, (_, point) => point - count),
    ]);
    traces.push([
      `runs ${String(count)}`,
      Float32Array.from(
        { length: count },
        (_, point) => Math.floor(point / 7) % 3,
      ),
    ]);
    traces.push([
      `all NaN ${String(count)}`,
      new Float32Array(count).fill(Number.NaN),
    ]);
  }
  traces.push([
    "not finite leaving",
    Float32Array.from({ length: 3 * divisor }, (_, point) =>
      point % 5 !== 0
        ? 0.5
        : point % 2 === 0
          ? Number.POSITIVE_INFINITY
          : Number.NaN,
    ),
  ]);

  test.each(traces)("matches the loop: %s", (_, values) => {
    const raw = {
      ...frame(FrameMode.raw, []),
      pointCount: values.length,
      pointsPerSample: 4,
    };
    expect(Array.from(cancellationLevels(raw, values))).toEqual(
      Array.from(levelsByLoop(raw, values)),
    );
    if (values.length % 2 === 0) {
      const columns = {
        ...frame(FrameMode.minMax, []),
        pointCount: values.length / 2,
      };
      expect(Array.from(cancellationLevels(columns, values))).toEqual(
        Array.from(levelsByLoop(columns, values)),
      );
    }
  });
});
