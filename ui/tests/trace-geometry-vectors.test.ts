// The geometry the macOS editor draws natively (lane boxes, the grid, Beats' musical lines,
// Compare's cancellation shading) must match the page's. These cases are computed from the page's
// helpers and kept in protocol/tests/data/trace-geometry.json, which the editor's C++ tests read;
// this test fails when the page's helpers no longer give them. After an intended change, write the
// file again with ORIZON_UPDATE_VECTORS=1 and review the diff.
import { readFileSync, writeFileSync } from "node:fs";
import { expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import { levelSteps, minimumLineGap } from "../src/view/axis-labels.ts";
import { gridLines } from "../src/view/grid.ts";
import { musicalGrid } from "../src/view/musical-grid.ts";
import { cancellationBands, lanes } from "../src/render/trace-geometry.ts";

const file = new URL(
  "../../protocol/tests/data/trace-geometry.json",
  import.meta.url,
);

const round = (value: number) => Math.round(value * 1e9) / 1e9;

function laneCases() {
  const cases = [];
  for (const layout of ["overlaid", "stacked"] as const) {
    for (let count = 1; count <= 16; count++) {
      cases.push({
        count,
        layout,
        lanes: lanes(count, ViewLayout[layout]).map((lane) => ({
          center: round(lane.center),
          halfHeight: round(lane.halfHeight),
        })),
      });
    }
  }
  return cases;
}

function levelStepCases() {
  return [0, 4, 7.9, 8, 10, 20, 40, 80, 200, 1000].map((halfLane) => ({
    halfLane,
    steps: levelSteps(halfLane, minimumLineGap),
  }));
}

function musicalCases() {
  const cases = [];
  for (const [cycleStart, span, bar] of [
    [0, 1, 4],
    [8, 4, 4],
    [16, 16, 4],
    [36, 36, 4], // Nine bars: strong lines off the bar grid.
    [3, 3, 3],
    [64, 64, 4],
    [0, 0.25, 4],
  ] as const) {
    for (const [start, end] of [
      [0, 1],
      [0.25, 0.75],
      [0.4, 0.41],
    ] as const) {
      cases.push({
        cycleStart,
        span,
        bar,
        viewStart: start,
        viewEnd: end,
        lines: musicalGrid(cycleStart, span, bar, { start, end }).lines.map(
          (line) => ({ position: round(line.position), strong: line.strong }),
        ),
      });
    }
  }
  return cases;
}

function gridCases() {
  const cases = [];
  for (const [width, height] of [
    [1002, 451],
    [2004, 902],
    [640, 300],
  ] as const) {
    for (const ratio of [1, 2, 1.5]) {
      for (const laneCount of [1, 2, 3, 15]) {
        cases.push({
          width,
          height,
          ratio,
          laneCount,
          lines: gridLines(width, height, laneCount, ratio),
        });
      }
    }
  }
  const musical = musicalGrid(16, 16, 4, { start: 0.1, end: 0.9 }).lines;
  cases.push({
    width: 1002,
    height: 451,
    ratio: 2,
    laneCount: 1,
    vertical: musical,
    lines: gridLines(1002, 451, 1, 2, musical),
  });
  return cases;
}

// A Compare frame of `points` min/max columns: two related tracks that cancel in their sum over
// the middle third, and the sum.
function compareFrame(points: number, mode: FrameMode): Frame {
  const perPoint = mode === FrameMode.minMax ? 2 : 1;
  const track = (sign: number) =>
    Float32Array.from({ length: points * perPoint }, (_, index) => {
      const point = Math.floor(index / perPoint);
      const level = 0.5 + 0.3 * Math.sin(point / 9);
      return perPoint === 2 ? (index % 2 === 0 ? -level : level) : sign * level;
    });
  const first = track(1);
  const second = track(1);
  const sum = Float32Array.from(first, (value, index) => {
    const point = Math.floor(index / perPoint);
    const cancelling = point > points / 3 && point < (2 * points) / 3;
    return cancelling ? value * 0.2 : value * 1.6;
  });
  return {
    mode,
    pointCount: points,
    startFraction: 0.25,
    viewStart: 0.1,
    viewEnd: 0.9,
    visibleSamples: points,
    lanes: [first, second, sum],
    comparison: {
      matches: [
        { correlation: 1, related: false, offset: Number.NaN, inverted: false },
        { correlation: 0.9, related: true, offset: 12, inverted: false },
      ],
    },
  } as unknown as Frame;
}

function cancellationCases() {
  const cases = [];
  for (const mode of [FrameMode.minMax, FrameMode.raw]) {
    for (const layout of ["overlaid", "stacked"] as const) {
      const frame = compareFrame(48, mode);
      const view = { start: 0.2, end: 0.8 };
      cases.push({
        mode: mode === FrameMode.minMax ? "minMax" : "raw",
        layout,
        pointCount: frame.pointCount,
        startFraction: frame.startFraction,
        frameViewStart: frame.viewStart,
        frameViewEnd: frame.viewEnd,
        visibleSamples: frame.visibleSamples,
        lanes: frame.lanes.map((values) => Array.from(values)),
        related: [false, true],
        compared: [true, true],
        viewStart: view.start,
        viewEnd: view.end,
        bands: Array.from(
          cancellationBands(
            frame,
            {
              widthPixels: 1000,
              heightPixels: 500,
              verticalScaleDb: 0,
              layout: ViewLayout[layout],
              lineWidthPixels: 3,
              view,
            },
            [true, true],
          ),
          round,
        ),
      });
    }
  }
  return cases;
}

const vectors = {
  description:
    "Geometry the macOS editor draws natively, computed from the page's helpers (ui/tests/trace-geometry-vectors.test.ts) and read by the editor's C++ tests. Write again with ORIZON_UPDATE_VECTORS=1.",
  lanes: laneCases(),
  levelSteps: levelStepCases(),
  musical: musicalCases(),
  grid: gridCases(),
  cancellation: cancellationCases(),
};

test("the page's geometry still gives the shared cases", () => {
  if (process.env["ORIZON_UPDATE_VECTORS"] === "1") {
    writeFileSync(file, `${JSON.stringify(vectors, null, 2)}\n`);
  }
  const kept: unknown = JSON.parse(readFileSync(file, "utf8"));
  expect(JSON.parse(JSON.stringify(vectors))).toEqual(kept);
});
