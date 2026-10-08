import { describe, expect, test } from "vitest";

import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  fullScale,
  cancellationBands,
  laneGeometry,
  lanes,
  traceGeometry,
  type TraceLayout,
} from "../src/render/trace-geometry.ts";

const layout: TraceLayout = {
  widthPixels: 1000,
  heightPixels: 500,
  verticalScaleDb: 0,
  layout: ViewLayout.overlaid,
  lineWidthPixels: 2,
};

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

describe("trace geometry", () => {
  test("magnifies the visible part of the frame", () => {
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [0, 0, 0, 0, 0, 0, 0, 0]]]),
      { ...layout, view: { start: 0.25, end: 0.5 } },
    ).get(ChannelView.left);
    // Column 1 of 4 spans frame 0.25 to 0.5, which now fills the screen.
    const column = xs(vertices?.subarray(12, 24) ?? new Float32Array());
    expect(Math.min(...column)).toBeCloseTo(-1);
    expect(Math.max(...column)).toBeCloseTo(1);
  });

  test("places a zoomed frame by the part of the span it covers", () => {
    // A frame rendered for the second quarter of the span, shown with that quarter on screen.
    const zoomed = {
      ...frame(FrameMode.raw, [[ChannelView.left, [0, 0, 0, 0, 0]]]),
      viewStart: 0.25,
      viewEnd: 0.5,
      visibleSamples: 4,
    };
    const vertices = traceGeometry(zoomed, {
      ...layout,
      view: { start: 0.25, end: 0.5 },
    }).get(ChannelView.left);
    expect(Math.min(...xs(vertices ?? new Float32Array()))).toBeCloseTo(-1, 2);
    expect(Math.max(...xs(vertices ?? new Float32Array()))).toBeCloseTo(1, 2);
  });

  test("stretches a frame rendered for an older view into the current one", () => {
    // Rendered for the whole span, shown zoomed to its first half: the frame's right half
    // lies beyond the screen's right edge.
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [0, 0, 0, 0]]]),
      { ...layout, view: { start: 0, end: 0.5 } },
    ).get(ChannelView.left);
    expect(Math.max(...xs(vertices ?? new Float32Array()))).toBeCloseTo(3);
  });

  test("draws one quad per min/max column spanning min to max", () => {
    const geometry = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [-0.5, 0.5, -1, 1]]]),
      layout,
    );
    const vertices = geometry.get(ChannelView.left);
    expect(vertices?.length).toBe(2 * 6 * 2);
    const firstColumn = ys(vertices?.subarray(0, 12) ?? new Float32Array());
    expect(Math.min(...firstColumn)).toBeCloseTo(-0.5);
    expect(Math.max(...firstColumn)).toBeCloseTo(0.5);
    expect(Math.min(...xs(vertices ?? new Float32Array()))).toBeCloseTo(-1);
    expect(Math.max(...xs(vertices ?? new Float32Array()))).toBeCloseTo(1);
  });

  test("joins columns that hold too few samples to overlap", () => {
    // A steep rise sampled once per column: each column is a single value.
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [
        [ChannelView.left, [-0.6, -0.6, -0.2, -0.2, 0.2, 0.2, 0.6, 0.6]],
      ]),
      layout,
    ).get(ChannelView.left);
    const columnYs = (column: number) =>
      ys(
        vertices?.subarray(column * 12, column * 12 + 12) ?? new Float32Array(),
      );
    const values = [-0.6, -0.2, 0.2, 0.6];
    for (let column = 1; column < 4; column++) {
      // Each column reaches down to the previous column's value, so the trace is continuous.
      expect(Math.min(...columnYs(column))).toBeCloseTo(
        values[column - 1] ?? 0,
      );
    }
  });

  test("keeps silent columns at least a line width tall", () => {
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [0, 0]]]),
      layout,
    ).get(ChannelView.left);
    const height =
      Math.max(...ys(vertices ?? new Float32Array())) -
      Math.min(...ys(vertices ?? new Float32Array()));
    expect(height * (layout.heightPixels / 2)).toBeCloseTo(
      layout.lineWidthPixels,
    );
  });

  test("maps the vertical scale in dBFS to the top of the screen", () => {
    expect(fullScale(0)).toBe(1);
    expect(fullScale(-20)).toBeCloseTo(0.1);
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [-0.1, 0.1]]]),
      {
        ...layout,
        verticalScaleDb: -20,
      },
    ).get(ChannelView.left);
    expect(Math.max(...ys(vertices ?? new Float32Array()))).toBeCloseTo(1, 4);
  });

  test("stacks views in equal lanes from the top", () => {
    expect(lanes(2, ViewLayout.stacked)).toEqual([
      { center: 0.5, halfHeight: 0.5 },
      { center: -0.5, halfHeight: 0.5 },
    ]);
    expect(lanes(2, ViewLayout.overlaid)).toEqual([
      { center: 0, halfHeight: 1 },
      { center: 0, halfHeight: 1 },
    ]);
  });

  test("draws raw samples as a thick line shifted by the start fraction", () => {
    const vertices = traceGeometry(
      frame(FrameMode.raw, [[ChannelView.mid, [0, 0, 0, 0]]], 0.5),
      layout,
    ).get(ChannelView.mid);
    expect(vertices?.length).toBe(3 * 6 * 2);
    // Sample 0 sits half a sample left of the screen edge.
    expect(Math.min(...xs(vertices ?? new Float32Array()))).toBeCloseTo(
      -1 - 0.25,
    );
    const thickness =
      Math.max(...ys(vertices ?? new Float32Array())) *
      2 *
      (layout.heightPixels / 2);
    expect(thickness).toBeCloseTo(layout.lineWidthPixels);
  });
});

describe("lane geometry", () => {
  test("stacks the lanes of a multi-track frame in lane order", () => {
    const multi = {
      ...frame(FrameMode.raw, [[ChannelView.left, [0, 0]]]),
      views: new Map(),
      lanes: [new Float32Array([1, 1]), new Float32Array([-1, -1])],
    };
    const geometry = laneGeometry(multi, {
      widthPixels: 200,
      heightPixels: 100,
      verticalScaleDb: 0,
      layout: ViewLayout.stacked,
      lineWidthPixels: 1,
    });
    expect(geometry).toHaveLength(2);
    // Top lane spans clip y 0 to 1; a full-scale line sits at its top, near y = 1.
    expect(Math.max(...ys(geometry[0] ?? new Float32Array()))).toBeGreaterThan(
      0.95,
    );
    // Bottom lane spans -1 to 0; a negative full-scale line sits at its bottom.
    expect(Math.min(...ys(geometry[1] ?? new Float32Array()))).toBeLessThan(
      -0.95,
    );
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
