// The geometry the macOS editor draws natively (lane boxes, the grid, Beats' musical lines,
// Compare's cancellation shading) must match the page's. These cases are computed from the page's
// helpers and kept in protocol/tests/data/trace-geometry.json, which the editor's C++ tests read;
// this test fails when the page's helpers no longer give them. After an intended change, write the
// file again with ORIZON_UPDATE_VECTORS=1 and review the diff.
import { readFileSync, writeFileSync } from "node:fs";
import { expect, test } from "vitest";

import {
  BeatDivision,
  FrameMode,
  GridDensity,
  GridType,
  ViewLayout,
} from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  dbFraction,
  levelLines,
  levelSteps,
  minimumLineGap,
} from "../src/view/axis-labels.ts";
import { gridColumns, gridLines } from "../src/view/grid.ts";
import type {
  BeatDivisionName,
  GridDensityName,
  GridTypeName,
} from "../src/view/grid-settings.ts";
import { beatSteps, musicalGrid } from "../src/view/musical-grid.ts";
import {
  gridCoarseLevelStride,
  gridCoarseLineGap,
  gridFineBeatLineGap,
  gridFineLevelSubdivisions,
  gridFineLineGap,
  gridLevelStepDb,
  gridLevelsBelowTop,
  gridMediumLineGap,
  cancellationBridgedPoints,
  cancellationQuietShare,
  cancellationReachDivisor,
} from "../../protocol/src/limits.ts";
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

const densities = Object.keys(GridDensity) as GridDensityName[];
const divisions = Object.keys(BeatDivision) as BeatDivisionName[];
const types = Object.keys(GridType) as GridTypeName[];

// The half-lanes at which a Level step's lines change, from the gaps: where its narrowest gap
// (the last line's to the one below) reaches the step's minimum; for Fine, where each 3 dB
// interval's narrowest 1 dB gap does.
function levelThresholds(): number[] {
  const gap = (upper: number, lower: number) =>
    dbFraction(upper) - dbFraction(lower);
  const bottom = gridLevelsBelowTop * gridLevelStepDb;
  const coarse = gridCoarseLevelStride * gridLevelStepDb;
  const minor = gridLevelStepDb / gridFineLevelSubdivisions;
  const thresholds = [
    gridMediumLineGap / gap(bottom - gridLevelStepDb, bottom),
    gridCoarseLineGap / gap(bottom - coarse, bottom),
    // The lowest line to the zero line.
    gridCoarseLineGap / dbFraction(bottom),
  ];
  for (let top = 0; top < bottom; top += gridLevelStepDb) {
    const lowest = top + gridLevelStepDb - minor;
    thresholds.push(gridFineLineGap / gap(lowest, lowest + minor));
  }
  return thresholds;
}

function levelLineCases() {
  const halfLanes = [
    ...[0, 4, 7.9, 8, 10, 20, 40, 80, 200, 1000],
    ...levelThresholds().flatMap((threshold) => [
      round(threshold - 0.5),
      round(threshold + 0.5),
    ]),
  ];
  return densities.flatMap((density) =>
    halfLanes.map((halfLane) => ({
      halfLane,
      density,
      lines: levelLines(halfLane, density),
    })),
  );
}

// Beats' lines at every Time step and Beat division. Fine is written at widths either side of its
// guard: where its finer step's lines stand gridFineBeatLineGap CSS pixels apart.
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
    // Two bars of 7/8 from bar 3: beats on whole quarter notes, so the odd bar line between them
    // is not drawn, as before this grid's choices (pinned, not endorsed).
    [7, 7, 3.5],
    // Eight bars of 3/4: bars at Medium, whose coarser levels (4 and 16 bars) are bars' multiples.
    [0, 24, 3],
  ] as const) {
    for (const [start, end] of [
      [0, 1],
      [0.25, 0.75],
      [0.4, 0.41],
    ] as const) {
      const visibleSpan = (end - start) * span;
      for (const division of divisions) {
        for (const time of densities) {
          const finer = beatSteps(visibleSpan, bar, {
            time: "fine",
            division,
            widthCss: Number.POSITIVE_INFINITY,
          }).step;
          const edge = (gridFineBeatLineGap * visibleSpan) / finer;
          const widths =
            time === "fine" ? [round(edge * 0.99), round(edge * 1.01)] : [1000];
          for (const widthCss of widths) {
            cases.push({
              cycleStart,
              span,
              bar,
              viewStart: start,
              viewEnd: end,
              time,
              division,
              widthCss,
              lines: musicalGrid(
                cycleStart,
                span,
                bar,
                { start, end },
                undefined,
                { time, division, widthCss },
              ).lines.map((line) => ({
                position: round(line.position),
                strong: line.strong,
              })),
            });
          }
        }
      }
    }
  }
  return cases;
}

// The Time and Level steps each type is written at: Grid at each step and each against the
// other's extremes; None and Ticks, whose zero lines no step changes, at one.
const steps: Readonly<
  Record<GridTypeName, readonly (readonly [GridDensityName, GridDensityName])[]>
> = {
  grid: [
    ...densities.map((density) => [density, density] as const),
    ["coarse", "fine"],
    ["fine", "coarse"],
  ],
  ticks: [["medium", "medium"]],
  none: [["medium", "medium"]],
};

// The grid of every type.
function gridCases() {
  const cases = [];
  for (const [width, height] of [
    [1002, 451],
    [2004, 902],
    [640, 300],
  ] as const) {
    for (const ratio of [1, 2, 1.5]) {
      for (const laneCount of [1, 2, 3, 15]) {
        for (const type of types) {
          for (const [time, level] of steps[type]) {
            const options = { type, columns: gridColumns[time], level };
            cases.push({
              width,
              height,
              ratio,
              laneCount,
              ...options,
              time,
              lines: gridLines(width, height, laneCount, ratio, options),
            });
          }
        }
      }
    }
  }
  const musical = musicalGrid(16, 16, 4, { start: 0.1, end: 0.9 }).lines;
  cases.push({
    width: 1002,
    height: 451,
    ratio: 2,
    laneCount: 1,
    type: "grid",
    columns: gridColumns.medium,
    level: "medium",
    time: "medium",
    vertical: musical,
    lines: gridLines(1002, 451, 1, 2, { verticalLines: musical }),
  });
  return cases;
}

// Where the sum of a Compare frame's tracks cancels:
// - "third": over the middle third;
// - "gaps": over the middle half, broken by a gap as long as the page bridges and one a point
//   longer, which splits it (raw points' levels are smoothed over their neighbours, which widens a
//   gap by as much each side, so their gaps are that much shorter);
// - "quiet": over a stretch where the tracks are quieter than cancellationQuietShare of the
//   loudest, which is not shaded, and over the last sixth, which is.
type CancellationShape = "third" | "gaps" | "quiet";

// A Compare frame of `points` columns or raw points: two related tracks and their sum, cancelling
// as `shape` says. `visibleSamples`: a raw window's own sample count, which a fractional zoom
// leaves different from its points; `missing`: a few of the first track's points not finite, as
// samples a host sent invalid are.
interface CompareOptions {
  readonly visibleSamples?: number;
  readonly missing?: boolean;
  // A deep view's points between samples: gaps bridged are that many times longer in points.
  readonly pointsPerSample?: number;
}

function compareFrame(
  points: number,
  mode: FrameMode,
  shape: CancellationShape = "third",
  options: CompareOptions = {},
): Frame {
  const perPoint = mode === FrameMode.minMax ? 2 : 1;
  const reach =
    mode === FrameMode.minMax
      ? 0
      : Math.max(1, Math.floor(points / cancellationReachDivisor));
  const quiet = (point: number) =>
    shape === "quiet" && point > points / 4 && point < points / 2;
  const track = (sign: number) =>
    Float32Array.from({ length: points * perPoint }, (_, index) => {
      const point = Math.floor(index / perPoint);
      const loud = 0.5 + 0.3 * Math.sin(point / 9);
      // Well under the quiet share of the loudest (0.8).
      const level = quiet(point) ? 0.4 * cancellationQuietShare : loud;
      return perPoint === 2 ? (index % 2 === 0 ? -level : level) : sign * level;
    });
  const bridged =
    cancellationBridgedPoints * (options.pointsPerSample ?? 1) - 2 * reach;
  const gaps = [
    { from: Math.floor(points * 0.35), length: bridged },
    { from: Math.floor(points * 0.6), length: bridged + 1 },
  ];
  const cancelling = (point: number): boolean => {
    switch (shape) {
      case "third":
        return point > points / 3 && point < (2 * points) / 3;
      case "gaps":
        return (
          point > points / 4 &&
          point < (3 * points) / 4 &&
          !gaps.some(
            (gap) => point >= gap.from && point < gap.from + gap.length,
          )
        );
      case "quiet":
        return quiet(point) || point > (5 * points) / 6;
    }
  };
  const first = track(1);
  if (options.missing === true) {
    // Outside the cancelling stretch: the shading there must not change.
    first.fill(Number.NaN, 4 * perPoint, 7 * perPoint);
  }
  const second = track(1);
  const sum = Float32Array.from(first, (value, index) =>
    cancelling(Math.floor(index / perPoint)) ? value * 0.2 : value * 1.6,
  );
  return {
    mode,
    pointCount: points,
    startFraction: 0.25,
    viewStart: 0.1,
    viewEnd: 0.9,
    visibleSamples: options.visibleSamples ?? points,
    pointsPerSample: options.pointsPerSample ?? 1,
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
    // Short frames, a long one (its gaps hundreds of points in, as real frames' are), a raw
    // window whose samples differ from its points, and a track with missing points.
    for (const [layout, shape, points, options] of [
      ["overlaid", "third", 48, {}],
      ["stacked", "third", 48, {}],
      ["stacked", "gaps", 48, {}],
      ["overlaid", "quiet", 48, {}],
      ["stacked", "gaps", 300, {}],
      ["overlaid", "third", 48, { visibleSamples: 38.6 }],
      ["overlaid", "third", 48, { missing: true }],
      ["stacked", "gaps", 400, { pointsPerSample: 8, visibleSamples: 50 }],
    ] as const) {
      if (mode === FrameMode.minMax && "pointsPerSample" in options) {
        continue; // Only raw frames carry points between samples.
      }
      const frame = compareFrame(points, mode, shape, options);
      const view = { start: 0.2, end: 0.8 };
      cases.push({
        mode: mode === FrameMode.minMax ? "minMax" : "raw",
        layout,
        pointCount: frame.pointCount,
        startFraction: frame.startFraction,
        frameViewStart: frame.viewStart,
        frameViewEnd: frame.viewEnd,
        visibleSamples: frame.visibleSamples,
        pointsPerSample: frame.pointsPerSample,
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
  levelLines: levelLineCases(),
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
