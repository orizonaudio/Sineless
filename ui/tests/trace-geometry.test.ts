import { describe, expect, test } from "vitest";

import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  fullScale,
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
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    views: new Map(
      views.map(([view, values]) => [view, new Float32Array(values)]),
    ),
  };
}

const ys = (vertices: Float32Array) =>
  vertices.filter((_, index) => index % 2 === 1);
const xs = (vertices: Float32Array) =>
  vertices.filter((_, index) => index % 2 === 0);

describe("trace geometry", () => {
  test("magnifies the visible part of the frame (FR-10)", () => {
    const vertices = traceGeometry(
      frame(FrameMode.minMax, [[ChannelView.left, [0, 0, 0, 0, 0, 0, 0, 0]]]),
      { ...layout, view: { start: 0.25, end: 0.5 } },
    ).get(ChannelView.left);
    // Column 1 of 4 spans frame 0.25 to 0.5, which now fills the screen.
    const column = xs(vertices?.subarray(12, 24) ?? new Float32Array());
    expect(Math.min(...column)).toBeCloseTo(-1);
    expect(Math.max(...column)).toBeCloseTo(1);
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
