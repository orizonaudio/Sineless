// The trace's shape as drawn by the GPU: smooth and anti-aliased at every slope and spacing,
// blended once, with gaps only where values are missing, and each point where hover and snap
// read it. Pixels are read back from a canvas of the test's own (trace-pixels.ts).
import { describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { maxLanes } from "../../../protocol/src/limits.ts";
import {
  columnPosition,
  coverageAttachments,
  lanes,
  pointPosition,
} from "../../src/render/trace-geometry.ts";
import type { Rgba } from "../../src/render/trace-renderer.ts";
import type { ViewWindow } from "../../src/view/zoom.ts";
import {
  columnCoverage,
  draw,
  drawOn,
  freshSurface,
  frameOf,
  traceFrame,
  type DrawOptions,
  type Drawing,
} from "./trace-pixels.ts";

const width = 400;
const height = 200;
const fullScreen: ViewWindow = { start: 0, end: 1 };
const byte = 1 / 255;

// Point spacings in device pixels: whole, either side of a 3 px line's reach (2 px), fractional
// as raw frames are (2560 px over 1000 samples), wide, and a frame stretched while zooming out:
// one, two or many pieces of the trace over each pixel.
const spacings = [1, 1.9, 2.1, 2.56, 4, 40, 0.5];
// The line widths at pixel ratios 1, 1.25, 1.5 and 2.
const lineWidths = [1.5, 1.875, 2.25, 3];

const valueAt = (y: number, h = height) => (2 * y) / h - 1;
const reachOf = (lineWidth: number) => lineWidth / 2 + 0.5;

// A frame of `count` points `spacing` px apart, centred on the screen: its window is sized so.
function spaced(
  mode: FrameMode,
  count: number,
  spacing: number,
  value: (point: number, x: number) => number | readonly [number, number],
  canvasWidth = width,
  content: Parameters<typeof traceFrame>[2] = {},
): Frame {
  const frameWidth = (count * spacing) / canvasWidth;
  const viewStart = (1 - frameWidth) / 2;
  const centre = (point: number) =>
    canvasWidth * viewStart +
    (mode === FrameMode.minMax ? point + 0.5 : point) * spacing;
  const values: number[] = [];
  for (let point = 0; point < count; point++) {
    const v = value(point, centre(point));
    if (mode === FrameMode.minMax) {
      const [lo, hi] = typeof v === "number" ? [v, v] : v;
      values.push(lo, hi);
    } else {
      values.push(typeof v === "number" ? v : v[0]);
    }
  }
  return traceFrame(mode, values, {
    viewStart,
    viewEnd: viewStart + frameWidth,
    ...content,
  });
}

// A point's centre in device pixels, as hover and snap read it.
function centreX(
  drawing: Drawing,
  point: number,
  screen: ViewWindow = fullScreen,
): number {
  const position =
    drawing.frame.mode === FrameMode.minMax
      ? columnPosition(drawing.frame, point + 0.5)
      : pointPosition(drawing.frame, point);
  return (
    ((position - screen.start) / (screen.end - screen.start)) * drawing.width
  );
}

function maxAlpha(
  drawing: Drawing,
  columns: Iterable<number> = range(0, drawing.width),
): number {
  let most = 0;
  for (const x of columns) {
    for (let y = 0; y < drawing.height; y++) {
      most = Math.max(most, drawing.alpha(x, y));
    }
  }
  return most;
}

function range(from: number, to: number): number[] {
  const out: number[] = [];
  for (let x = Math.ceil(from); x < to; x++) {
    out.push(x);
  }
  return out;
}

// The pixel columns at least `margin` px inside the first and last point.
function interior(drawing: Drawing, margin: number): number[] {
  const first = centreX(drawing, 0);
  const last = centreX(drawing, drawing.frame.pointCount - 1);
  return range(
    Math.max(first + margin, 0),
    Math.min(last - margin, drawing.width),
  );
}

// Alpha-weighted centroid of a pixel row between two columns, in pixel coordinates.
function rowCentroid(
  drawing: Drawing,
  y: number,
  from = 0,
  to = drawing.width,
): number | undefined {
  let sum = 0;
  let weighted = 0;
  for (const x of range(from, to)) {
    const a = drawing.alpha(x, y);
    sum += a;
    weighted += a * (x + 0.5);
  }
  return sum > 0 ? weighted / sum : undefined;
}

describe("a steep min/max flank follows the line, with no steps", () => {
  // Cells 4 px wide, so a staircase would put each row's centroid up to 2 px off the line.
  const spacing = 4;
  const cases = [
    { name: "rising, spans meeting", rise: 5, span: 5 },
    { name: "falling, spans meeting", rise: -5, span: 5 },
    { name: "rising past each column's span", rise: 8, span: 2 },
    { name: "falling past each column's span", rise: -8, span: 2 },
  ];
  test.each(cases)("$name", ({ rise, span }) => {
    const count = 20;
    const start = rise > 0 ? 30 : height - 30;
    const frame = spaced(FrameMode.minMax, count, spacing, (point) => {
      const lo = start + rise * point - span / 2;
      return [valueAt(lo), valueAt(lo + span)];
    });
    const drawing = draw(frame);
    expect(drawing.error).toBe(0);
    // Between the second and the second-to-last column centres, away from the end caps.
    const centre = (point: number) => start + rise * point;
    const [low, high] = [centre(1), centre(count - 2)].sort((a, b) => a - b);
    for (let y = Math.ceil(low ?? 0); y < (high ?? 0); y++) {
      const row = y + 0.5;
      const ideal = centreX(drawing, 0) + ((row - start) / rise) * spacing;
      const found = rowCentroid(drawing, y);
      expect(found, `row ${String(y)} is drawn`).toBeDefined();
      expect(Math.abs((found ?? 0) - ideal), `row ${String(y)}`).toBeLessThan(
        0.5,
      );
    }
  });
});

describe("a trace is equally thick across the line at every slope", () => {
  const slopes = [0, 1, 4];
  const modes = [
    { name: "raw", mode: FrameMode.raw },
    { name: "min/max", mode: FrameMode.minMax },
  ];
  const cases = modes.flatMap(({ name, mode }) =>
    slopes.flatMap((slope) =>
      spacings.flatMap((spacing) =>
        lineWidths.map((lineWidth) => ({
          name,
          mode,
          slope,
          spacing,
          lineWidth,
        })),
      ),
    ),
  );
  // Tall enough that a slope of 4 still spans a few wide-spaced points.
  const tall = 800;
  test.each(cases)(
    "$name at slope $slope, $spacing px apart, $lineWidth px wide",
    ({ mode, slope, spacing, lineWidth }) => {
      const reach = (tall / 2 - 20) / Math.max(slope, 0.01);
      const span = Math.min(2 * reach, width * 0.9);
      const count = Math.max(2, Math.floor(span / spacing) + 1);
      const frame = spaced(mode, count, spacing, (_, x) =>
        valueAt(tall / 2 + slope * (x - width / 2), tall),
      );
      const drawing = draw(frame, { height: tall, lineWidthPixels: lineWidth });
      expect(drawing.error).toBe(0);
      const columns = interior(drawing, reachOf(lineWidth) + 1);
      expect(columns.length).toBeGreaterThan(0);
      for (const x of columns) {
        const across = columnCoverage(drawing, x) * Math.cos(Math.atan(slope));
        expect(
          Math.abs(across - lineWidth) / lineWidth,
          `column ${String(x)}`,
        ).toBeLessThan(0.15);
      }
    },
  );

  // Points far closer than a pixel, as a frame stretched far while zooming out: about 60 pieces
  // over each pixel, still one line's thickness.
  test("a frame stretched to points 0.06 px apart is as thick as the line", () => {
    const lineWidth = 3;
    const spacing = 0.06;
    const count = Math.floor((width * 0.8) / spacing);
    const frame = spaced(FrameMode.raw, count, spacing, () => 0);
    const drawing = draw(frame, { lineWidthPixels: lineWidth });
    expect(drawing.error).toBe(0);
    for (const x of interior(drawing, reachOf(lineWidth) + 1)) {
      expect(
        Math.abs(columnCoverage(drawing, x) - lineWidth) / lineWidth,
        `column ${String(x)}`,
      ).toBeLessThan(0.15);
    }
  });
});

test("edges are anti-aliased, with many alpha levels across a diagonal edge", () => {
  const frame = spaced(FrameMode.raw, 90, 4, (_, x) =>
    valueAt(height / 2 + 0.3 * (x - width / 2)),
  );
  const drawing = draw(frame);
  const levels = new Set<number>();
  for (let x = 0; x < width; x++) {
    for (let y = 0; y < height; y++) {
      const a = drawing.pixel(x, y)[3];
      if (a > 0 && a < 255) {
        levels.add(a);
      }
    }
  }
  expect(levels.size).toBeGreaterThanOrEqual(8);
});

describe("a dimmed or faint trace blends once at its joins", () => {
  // A third of the points flat, where nothing overlaps, then a zigzag.
  const amplitude = 20;
  const cases = [FrameMode.raw, FrameMode.minMax].flatMap((mode) =>
    spacings.flatMap((spacing) => [
      { mode, spacing, translucent: true },
      { mode, spacing, translucent: false },
    ]),
  );
  test.each(cases)(
    "mode $mode, $spacing px apart, translucent $translucent (else dimmed)",
    ({ mode, spacing, translucent }) => {
      const count = Math.floor((width * 0.9) / spacing);
      const flat = Math.floor(count / 3);
      // On a pixel row's centre, so the flat run reaches the colour's full alpha.
      const level = (point: number) =>
        point < flat
          ? height / 2 + 0.5
          : height / 2 + (point % 2 === 0 ? amplitude : -amplitude);
      const frame = spaced(mode, count, spacing, (point) =>
        mode === FrameMode.minMax
          ? [valueAt(level(point) - 2), valueAt(level(point) + 2)]
          : valueAt(level(point)),
      );
      const options: DrawOptions = translucent
        ? { color: [1, 1, 1, 0.5] }
        : { dim: true };
      const drawing = draw(frame, options);
      // The flat run, from the first point to the last before the zigzag, away from both ends.
      const flatColumns = range(
        centreX(drawing, 0) + 3,
        centreX(drawing, flat - 1) - 3,
      );
      const single = maxAlpha(drawing, flatColumns);
      expect(single).toBeGreaterThan(0.3);
      expect(maxAlpha(drawing)).toBeLessThanOrEqual(single + byte);
    },
  );
});

describe("lanes are drawn apart", () => {
  const colors: Rgba[] = [
    [1, 0, 0, 1],
    [0, 1, 0, 1],
    [0, 0, 1, 1],
  ];

  test("a lane never joins the previous lane's last value, and every lane draws", () => {
    const count = 60;
    const spacing = 4;
    const frameWidth = (count * spacing) / width;
    // Each lane at its own level, so none hides another.
    const levels = [-0.9, 0.9, -0.5];
    const frame = frameOf({
      mode: FrameMode.minMax,
      pointCount: count,
      lanes: levels.map((level) =>
        Array.from({ length: count * 2 }, () => level),
      ),
      viewStart: (1 - frameWidth) / 2,
      viewEnd: (1 + frameWidth) / 2,
    });
    const drawing = draw(frame, { laneColors: colors });
    expect(drawing.error).toBe(0);
    const yOf = (value: number) => ((value + 1) / 2) * height;
    levels.forEach((level, lane) => {
      const y = Math.floor(yOf(level));
      const drawn = range(0, width).some(
        (x) => (drawing.pixel(x, y)[lane] ?? 0) > 200,
      );
      expect(drawn, `lane ${String(lane)}`).toBe(true);
    });
    const reach = reachOf(3);
    // Between the levels, where a join from one lane's last value to the next's first would run.
    for (
      let y = Math.ceil(yOf(-0.5) + reach + 1);
      y < yOf(0.9) - reach - 1;
      y++
    ) {
      for (let x = 0; x < width; x++) {
        expect(drawing.alpha(x, y), `${String(x)}, ${String(y)}`).toBe(0);
      }
    }
  });

  test("stacked lanes stay in their lanes", () => {
    const count = 200;
    const frame = frameOf({
      mode: FrameMode.raw,
      pointCount: count,
      lanes: colors.map((_, lane) =>
        Array.from(
          { length: count },
          (_value, point) => 0.8 * Math.sin(point / (5 + lane)),
        ),
      ),
    });
    const drawing = draw(frame, {
      laneColors: colors,
      layout: ViewLayout.stacked,
    });
    const reach = reachOf(3);
    const placed = lanes(colors.length, ViewLayout.stacked);
    for (let x = 0; x < width; x++) {
      for (let y = 0; y < height; y++) {
        const pixel = drawing.pixel(x, y);
        if (pixel[3] === 0) {
          continue;
        }
        const lane = [0, 1, 2].reduce((best, channel) =>
          (pixel[channel] ?? 0) > (pixel[best] ?? 0) ? channel : best,
        );
        const { center, halfHeight } = placed[lane] ?? {
          center: 0,
          halfHeight: 1,
        };
        const bottom = ((center - halfHeight + 1) / 2) * height;
        const top = ((center + halfHeight + 1) / 2) * height;
        expect(y + 0.5).toBeGreaterThan(bottom - reach);
        expect(y + 0.5).toBeLessThan(top + reach);
      }
    }
  });
});

describe("silence and lone values still draw; values that are not finite leave a gap", () => {
  test("a silent min/max frame draws a line at the centre", () => {
    const drawing = draw(spaced(FrameMode.minMax, 300, 1, () => 0));
    for (const x of interior(drawing, 3)) {
      expect(columnCoverage(drawing, x)).toBeCloseTo(3, 0);
    }
  });

  test("a raw frame of one point draws a dot", () => {
    const frame = traceFrame(FrameMode.raw, [0], {
      viewStart: 0.5,
      viewEnd: 0.6,
    });
    const drawing = draw(frame);
    expect(drawing.alpha(width / 2, height / 2)).toBeGreaterThan(0.5);
  });

  const level = 0.3;
  const bad = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];
  const where = ["min", "max", "both"] as const;
  const cases = spacings.flatMap((spacing) =>
    bad.flatMap((value) => [
      ...where.map((part) => ({
        mode: FrameMode.minMax,
        spacing,
        value,
        part,
      })),
      { mode: FrameMode.raw, spacing, value, part: "value" as const },
    ]),
  );
  test.each(cases)(
    "mode $mode, $value in its $part, $spacing px apart",
    ({ mode, spacing, value, part }) => {
      const count = Math.min(15, Math.floor((width * 0.9) / spacing));
      const middle = Math.floor(count / 2);
      const frame = spaced(mode, count, spacing, (point) => {
        if (point !== middle) {
          return level;
        }
        return part === "min"
          ? [value, level]
          : part === "max"
            ? [level, value]
            : [value, value];
      });
      const lineWidth = 3;
      const reach = reachOf(lineWidth);
      const drawing = draw(frame, { lineWidthPixels: lineWidth });
      expect(drawing.error).toBe(0);
      const y = ((level + 1) / 2) * height;
      for (let row = 0; row < height; row++) {
        for (let x = 0; x < width; x++) {
          if (drawing.alpha(x, row) > 0) {
            expect(
              Math.abs(row + 0.5 - y),
              `${String(x)}, ${String(row)}`,
            ).toBeLessThan(reach + 1);
          }
        }
      }
      if (spacing > reach) {
        const centre = centreX(drawing, middle);
        for (const x of range(
          centre - (spacing - reach) - 0.5,
          centre + (spacing - reach) - 0.5,
        )) {
          if (Math.abs(x + 0.5 - centre) < spacing - reach) {
            expect(columnCoverage(drawing, x), `column ${String(x)}`).toBe(0);
          }
        }
      }
    },
  );
});

describe("far off-scale values are drawn along the true line", () => {
  // At −40 dB a value of 1 sits 50 lane heights above the screen.
  const options: DrawOptions = { verticalScaleDb: -40 };
  const spacing = 40;
  const lineY = (value: number) => ((value / 0.01 + 1) / 2) * height;

  test.each([
    { name: "from the screen to 100 lane heights above", from: 0, to: 2 },
    { name: "from 50 lane heights above to 50 below", from: 1, to: -1 },
  ])("$name", ({ from, to }) => {
    const frame = spaced(FrameMode.raw, 4, spacing, (point) =>
      point % 2 === 0 ? from : to,
    );
    const drawing = draw(frame, options);
    expect(drawing.error).toBe(0);
    const [ya, yb] = [lineY(from), lineY(to)];
    const xa = centreX(drawing, 0);
    const reach = reachOf(3);
    let rows = 0;
    // The rows the first segment crosses, away from its points' own dots.
    for (let y = 0; y < height; y++) {
      const row = y + 0.5;
      if (row < Math.min(ya, yb) + 4 || row > Math.max(ya, yb) - 4) {
        continue;
      }
      const ideal = xa + ((row - ya) / (yb - ya)) * spacing;
      const found = rowCentroid(
        drawing,
        y,
        ideal - reach - 2,
        ideal + reach + 2,
      );
      rows += 1;
      expect(found, `row ${String(y)} is drawn`).toBeDefined();
      expect(Math.abs((found ?? 0) - ideal), `row ${String(y)}`).toBeLessThan(
        0.5,
      );
    }
    expect(rows).toBeGreaterThan(0);
  });

  test("adjacent points at +3e38 and −3e38 draw a stroke across the whole screen", () => {
    const frame = spaced(FrameMode.raw, 6, spacing, (point) =>
      point === 2 ? 3e38 : point === 3 ? -3e38 : 0,
    );
    const drawing = draw(frame);
    expect(drawing.error).toBe(0);
    const reach = reachOf(3);
    const middle = (centreX(drawing, 2) + centreX(drawing, 3)) / 2;
    for (let y = 0; y < height; y++) {
      const crossing = range(0, width).some(
        (x) => Math.abs(x + 0.5 - middle) < reach && drawing.alpha(x, y) > 0.9,
      );
      expect(crossing, `row ${String(y)}`).toBe(true);
    }
  });

  test.each([1e30, 3e38])(
    "a value of %f draws a stroke leaving the screen",
    (value) => {
      const count = 9;
      const middle = 4;
      const frame = spaced(FrameMode.raw, count, spacing, (point) =>
        point === middle ? value : 0,
      );
      const drawing = draw(frame);
      expect(drawing.error).toBe(0);
      const reach = reachOf(3);
      const before = centreX(drawing, middle - 1);
      const after = centreX(drawing, middle + 1);
      // Above the trace, only the two strokes up to it, rising from its neighbours.
      for (let y = Math.ceil(height / 2 + reach + 1); y < height; y++) {
        let drawn = 0;
        for (let x = 0; x < width; x++) {
          if (drawing.alpha(x, y) > 0) {
            drawn += 1;
            const near =
              Math.abs(x + 0.5 - before) < reach + 1 ||
              Math.abs(x + 0.5 - after) < reach + 1;
            expect(near, `${String(x)}, ${String(y)}`).toBe(true);
          }
        }
        expect(drawn, `row ${String(y)}`).toBeGreaterThan(0);
      }
    },
  );
});

describe("min/max bands far off-scale are filled correctly", () => {
  // At −40 dB a value of 2 sits 100 lane heights above the screen.
  const options: DrawOptions = { verticalScaleDb: -40 };
  const scale = 0.01;
  const reach = reachOf(3);
  // A value's height in device pixels, guarded as the renderer guards it.
  const pixels = (value: number) =>
    Math.min(Math.max((value / scale) * (height / 2), -1e15), 1e15) +
    height / 2;
  const count = (spacing: number) =>
    Math.min(41, Math.floor((width * 0.8) / spacing) | 1);

  // Every pixel between the interpolated lo and hi polylines, at least 1 px from them, is solid.
  function checkSolid(
    drawing: Drawing,
    bands: readonly (readonly [number, number])[],
    spacing: number,
  ): number {
    const first = centreX(drawing, 0);
    let checked = 0;
    for (const x of interior(drawing, 0.5)) {
      const t = (x + 0.5 - first) / spacing;
      const i = Math.min(Math.max(Math.floor(t), 0), bands.length - 2);
      const f = Math.min(Math.max(t - i, 0), 1);
      const [loA, hiA] = bands[i] ?? [0, 0];
      const [loB, hiB] = bands[i + 1] ?? [0, 0];
      const lo = pixels(loA) + (pixels(loB) - pixels(loA)) * f;
      const hi = pixels(hiA) + (pixels(hiB) - pixels(hiA)) * f;
      for (
        let y = Math.max(0, Math.ceil(lo + 0.5));
        y < height && y + 0.5 < hi - 1;
        y++
      ) {
        expect(
          drawing.alpha(x, y),
          `${String(x)}, ${String(y)}`,
        ).toBeGreaterThanOrEqual(1 - byte);
        checked += 1;
      }
    }
    return checked;
  }

  const spacingsHere = [1, 2.1, 4, 40];
  test.each(spacingsHere)(
    "lo and hi beyond opposite edges fill the screen, %f px apart",
    (spacing) => {
      const n = count(spacing);
      const bands = Array.from({ length: n }, () => [-2, 2] as const);
      const drawing = draw(
        spaced(FrameMode.minMax, n, spacing, () => [-2, 2]),
        options,
      );
      expect(drawing.error).toBe(0);
      for (const x of interior(drawing, 0.5)) {
        expect(
          columnCoverage(drawing, x),
          `column ${String(x)}`,
        ).toBeGreaterThanOrEqual(height * (1 - byte));
      }
      expect(checkSolid(drawing, bands, spacing)).toBeGreaterThan(0);
    },
  );

  const lone = spacingsHere.flatMap((spacing) =>
    [
      { name: "hi beyond the top", band: [0, 2] as const },
      { name: "lo beyond the bottom", band: [-2, 0] as const },
      { name: "hi at 3e38", band: [0, 3e38] as const },
    ].map((shape) => ({ ...shape, spacing })),
  );
  test.each(lone)(
    "one column with $name is solid within its envelope, $spacing px apart",
    ({ band, spacing }) => {
      const n = count(spacing);
      const middle = Math.floor(n / 2);
      const bands = Array.from({ length: n }, (_, point) =>
        point === middle ? band : ([0, 0] as const),
      );
      const drawing = draw(
        spaced(FrameMode.minMax, n, spacing, (point) => bands[point] ?? [0, 0]),
        options,
      );
      expect(drawing.error).toBe(0);
      expect(checkSolid(drawing, bands, spacing)).toBeGreaterThan(0);
      // An opaque white trace: every pixel is grey, never another colour.
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          const [r, g, b, a] = drawing.pixel(x, y);
          expect([r, g, b], `${String(x)}, ${String(y)}`).toEqual([a, a, a]);
        }
      }
    },
  );

  test.each(spacingsHere)(
    "both beyond the top draw only the strokes to the neighbours, %f px apart",
    (spacing) => {
      const n = count(spacing);
      const middle = Math.floor(n / 2);
      const drawing = draw(
        spaced(FrameMode.minMax, n, spacing, (point) =>
          point === middle ? [1.5, 2] : [0, 0],
        ),
        options,
      );
      expect(drawing.error).toBe(0);
      const before = centreX(drawing, middle - 1);
      const after = centreX(drawing, middle + 1);
      // The strokes leave the screen within a third of a pixel of the neighbours' x.
      for (let y = Math.ceil(height / 2 + reach + 1); y < height; y++) {
        let drawn = 0;
        for (let x = 0; x < width; x++) {
          if (drawing.alpha(x, y) > 0) {
            drawn += 1;
            const near =
              Math.abs(x + 0.5 - before) < reach + 1.5 ||
              Math.abs(x + 0.5 - after) < reach + 1.5;
            expect(near, `${String(x)}, ${String(y)}`).toBe(true);
          }
        }
        expect(drawn, `row ${String(y)}`).toBeGreaterThan(0);
      }
    },
  );
});

describe("each point is drawn where hover and snap read it", () => {
  const cases = [
    { startFraction: 0, visibleSamples: 12, screen: fullScreen },
    { startFraction: 0.5, visibleSamples: 12, screen: fullScreen },
    { startFraction: 0.99, visibleSamples: 12, screen: fullScreen },
    { startFraction: 0.5, visibleSamples: 10, screen: fullScreen },
    {
      startFraction: 0.5,
      visibleSamples: 12,
      screen: { start: 0.1, end: 0.9 },
    },
  ];
  test.each(cases)(
    "raw, start fraction $startFraction, $visibleSamples visible, screen $screen.start to $screen.end",
    ({ startFraction, visibleSamples, screen }) => {
      // Every other point missing, so each present one is a dot of its own.
      const values = Array.from({ length: 12 }, (_, point) =>
        point % 2 === 0 ? 0.2 : Number.NaN,
      );
      const frame = traceFrame(FrameMode.raw, values, {
        startFraction,
        visibleSamples,
        viewStart: 0.05,
        viewEnd: 0.95,
      });
      const drawing = draw(frame, { view: screen });
      checkDots(drawing, screen, 12);
    },
  );

  test("min/max columns under a narrower screen", () => {
    const values = Array.from({ length: 12 }, (_, point) =>
      point % 2 === 0 ? [0.2, 0.2] : [Number.NaN, Number.NaN],
    ).flat();
    const screen = { start: 0.1, end: 0.9 };
    const frame = traceFrame(FrameMode.minMax, values, {
      viewStart: 0.05,
      viewEnd: 0.95,
    });
    checkDots(draw(frame, { view: screen }), screen, 12);
  });

  function checkDots(
    drawing: Drawing,
    screen: ViewWindow,
    count: number,
  ): void {
    const y = ((0.2 + 1) / 2) * height;
    let checked = 0;
    for (let point = 0; point < count; point += 2) {
      const x = centreX(drawing, point, screen);
      if (x < 10 || x > width - 10) {
        continue;
      }
      let sum = 0;
      let weighted = 0;
      for (let row = Math.floor(y - 8); row < y + 8; row++) {
        for (const column of range(x - 8, x + 8)) {
          const a = drawing.alpha(column, row);
          sum += a;
          weighted += a * (column + 0.5);
        }
      }
      expect(sum, `point ${String(point)} is drawn`).toBeGreaterThan(0);
      expect(
        Math.abs(weighted / sum - x),
        `point ${String(point)}`,
      ).toBeLessThan(0.25);
      checked += 1;
    }
    expect(checked).toBeGreaterThan(2);
  }

  test("the first and last points' round ends are drawn whole", () => {
    const lineWidth = 6;
    const frame = spaced(FrameMode.raw, 200, 1, () => 0);
    const drawing = draw(frame, { lineWidthPixels: lineWidth });
    const y = height / 2;
    const first = centreX(drawing, 0);
    const last = centreX(drawing, 199);
    expect(drawing.alpha(Math.floor(first - 2), y)).toBeGreaterThan(0.9);
    expect(drawing.alpha(Math.floor(last + 2), y)).toBeGreaterThan(0.9);
  });
});

describe("joins on pixel centres shade once", () => {
  // The trace on a pixel row's centre, so a 3 px line covers that row fully.
  const centre = height / 2 + 0.5;
  const count = 300;
  test.each([
    { name: "a flat raw trace", mode: FrameMode.raw, spikes: false },
    { name: "a flat min/max trace", mode: FrameMode.minMax, spikes: false },
    {
      name: "a min/max trace with bands around it every fourth column",
      mode: FrameMode.minMax,
      spikes: true,
    },
  ])("$name", ({ mode, spikes }) => {
    const values = Array.from({ length: count }, (_, point) => {
      const half = spikes && point % 4 === 0 ? 10 : 0;
      return mode === FrameMode.minMax
        ? [valueAt(centre - half), valueAt(centre + half)]
        : [valueAt(centre)];
    }).flat();
    // Cell edges at x.5, on pixel centres: min/max columns from x = 50.5, raw points at whole
    // pixels from x = 50, each cell half a pixel either side.
    const viewStart = (mode === FrameMode.minMax ? 50.5 : 50) / width;
    const frame = traceFrame(mode, values, {
      viewStart,
      viewEnd: viewStart + count / width,
    });
    const drawing = draw(frame, { color: [1, 1, 1, 0.5], lineWidthPixels: 3 });
    expect(drawing.error).toBe(0);
    for (const x of range(60, 340)) {
      expect(
        Math.abs(drawing.alpha(x, Math.floor(centre)) - 0.5),
        `column ${String(x)}`,
      ).toBeLessThanOrEqual(byte);
    }
  });
});

describe("frames longer than one texture row draw across the seams", () => {
  const wide = 2100;
  const ramp = (count: number) => (point: number) =>
    -0.8 + (1.6 * point) / (count - 1);

  function checkRamp(drawing: Drawing, value: (x: number) => number): void {
    for (const x of interior(drawing, 3)) {
      let sum = 0;
      let weighted = 0;
      for (let y = 0; y < drawing.height; y++) {
        const a = drawing.alpha(x, y);
        sum += a;
        weighted += a * (y + 0.5);
      }
      expect(sum, `column ${String(x)}`).toBeGreaterThan(1.5);
      const ideal = ((value(x + 0.5) + 1) / 2) * drawing.height;
      expect(
        Math.abs(weighted / sum - ideal),
        `column ${String(x)}`,
      ).toBeLessThan(1);
    }
  }

  const cases = [
    ...[2047, 2048, 2049, 4096, 4097].map((count) => ({
      mode: FrameMode.raw,
      count,
    })),
    ...[1023, 1024, 1025, 2048, 2049].map((count) => ({
      mode: FrameMode.minMax,
      count,
    })),
  ];
  test.each(cases)("mode $mode, $count points", ({ mode, count }) => {
    const spacing = (wide * 0.95) / count;
    const shape = ramp(count);
    const frame = spaced(mode, count, spacing, (point) => shape(point), wide);
    const drawing = draw(frame, { width: wide });
    expect(drawing.error).toBe(0);
    const first = centreX(drawing, 0);
    checkRamp(drawing, (x) => shape((x - first) / spacing));
  });

  test("a second lane starting mid-row", () => {
    const count = 1500;
    const shape = ramp(count);
    const frame = frameOf({
      mode: FrameMode.raw,
      pointCount: count,
      lanes: [0, 1].map(() =>
        Array.from({ length: count }, (_, p) => shape(p)),
      ),
      viewStart: 0,
      viewEnd: 1,
    });
    const drawing = draw(frame, {
      width: wide,
      layout: ViewLayout.stacked,
      laneColors: [
        [1, 0, 0, 1],
        [0, 1, 0, 1],
      ],
    });
    // The second lane, in the bottom half: its ramp runs through all its columns.
    for (let x = 4; x < wide - 4; x++) {
      const point = (x + 0.5) / (wide / count);
      const value = shape(point);
      const y = Math.floor(((value * 0.5 - 0.5 + 1) / 2) * height);
      const near = [y - 1, y, y + 1].some(
        (row) => drawing.pixel(x, row)[1] > 100,
      );
      expect(near, `column ${String(x)}`).toBe(true);
    }
  });

  test("a short frame after a long one draws only its own data, and a long one after it all", () => {
    const long = 4097;
    const short = 100;
    const longFrame = () =>
      spaced(FrameMode.raw, long, (wide * 0.95) / long, ramp(long), wide);
    draw(longFrame(), { width: wide });
    const shortFrame = spaced(FrameMode.raw, short, 2, () => 0, wide);
    const drawing = draw(shortFrame, { width: wide });
    const first = centreX(drawing, 0);
    const last = centreX(drawing, short - 1);
    for (let x = 0; x < wide; x++) {
      if (x + 0.5 < first - 3 || x + 0.5 > last + 3) {
        expect(columnCoverage(drawing, x), `column ${String(x)}`).toBe(0);
      }
    }
    const again = draw(longFrame(), { width: wide });
    const spacing = (wide * 0.95) / long;
    const start = centreX(again, 0);
    checkRamp(again, (x) => ramp(long)((x - start) / spacing));
  });
});

describe("a min/max band's interior is solid", () => {
  const count = (spacing: number) =>
    Math.min(60, Math.floor((width * 0.9) / spacing));
  const shapes = [
    { name: "a constant band", half: () => 0.5 },
    {
      name: "an alternating span",
      half: (point: number) => (point % 2 === 0 ? 0.5 : 0.3),
    },
    {
      name: "an envelope narrowing then widening",
      half: (point: number, n: number) =>
        0.1 + (0.4 * Math.abs(point - n / 2)) / (n / 2),
    },
  ];
  const cases = shapes.flatMap((shape) =>
    [1, 2.1, 4, 40].map((spacing) => ({ ...shape, spacing })),
  );
  test.each(cases)("$name, $spacing px apart", ({ half, spacing }) => {
    const n = count(spacing);
    const frame = spaced(FrameMode.minMax, n, spacing, (point) => [
      -half(point, n),
      half(point, n),
    ]);
    const opaque = draw(frame);
    const first = centreX(opaque, 0);
    const halfAt = (x: number) => {
      const t = (x - first) / spacing;
      const i = Math.min(Math.max(Math.floor(t), 0), n - 2);
      const f = Math.min(Math.max(t - i, 0), 1);
      return half(i, n) * (1 - f) + half(i + 1, n) * f;
    };
    for (const x of interior(opaque, 0.5)) {
      const extent = (halfAt(x + 0.5) * height) / 2;
      for (
        let y = Math.ceil(height / 2 - extent + 1);
        y + 0.5 < height / 2 + extent - 1;
        y++
      ) {
        expect(
          opaque.alpha(x, y),
          `${String(x)}, ${String(y)}`,
        ).toBeGreaterThanOrEqual(1 - byte);
      }
    }
    const translucent = draw(frame, { color: [1, 1, 1, 0.5] });
    const middle = Math.max(
      ...interior(translucent, 0.5).map((x) =>
        translucent.alpha(x, height / 2),
      ),
    );
    expect(maxAlpha(translucent)).toBeLessThanOrEqual(middle + byte);
  });
});

// Traces are layered over each other in lane order, later over earlier, as the canvas blends one
// trace after another. Premultiplied, as the canvas holds its pixels (0 to 1).
type Pixel = readonly [number, number, number, number];
const transparent: Pixel = [0, 0, 0, 0];
function over(below: Pixel, color: Rgba): Pixel {
  const a = color[3];
  return [
    color[0] * a + below[0] * (1 - a),
    color[1] * a + below[1] * (1 - a),
    color[2] * a + below[2] * (1 - a),
    a + below[3] * (1 - a),
  ];
}
// A colour of its own for each lane, unlike its neighbours'.
const laneColor = (lane: number, alpha = 1): Rgba => [
  (lane % 4) / 3,
  (Math.floor(lane / 4) % 4) / 3,
  lane % 2 === 0 ? 1 : 0.25,
  alpha,
];
function expectPixel(
  drawing: Drawing,
  x: number,
  y: number,
  expected: Pixel,
  tolerance: number,
): void {
  const actual = drawing.pixel(x, y);
  expected.forEach((value, channel) => {
    expect(
      Math.abs((actual[channel] ?? 0) - value * 255),
      `${String(x)}, ${String(y)} channel ${String(channel)}: ${String(actual)}`,
    ).toBeLessThanOrEqual(tolerance);
  });
}
// Flat overlaid lanes, each at its own level, across the whole screen.
function flatLanes(levels: readonly number[]): Frame {
  const count = 100;
  return frameOf({
    mode: FrameMode.minMax,
    pointCount: count,
    viewStart: 0,
    viewEnd: 1,
    lanes: levels.map((level) =>
      Array.from({ length: count * 2 }, () => level),
    ),
  });
}
const rowOf = (value: number) => Math.floor(((value + 1) / 2) * height);

describe("lanes layer in order, whatever their channel and target", () => {
  // Each count of targets in use, either side of each target's last channel, and the most lanes.
  const counts = [
    ...new Set([
      ...Array.from({ length: coverageAttachments }, (_, target) => [
        4 * target + 1,
        4 * target + 4,
      ]).flat(),
      maxLanes,
    ]),
  ].filter((count) => count <= maxLanes);
  const spread = (count: number) =>
    Array.from({ length: count }, (_, lane) =>
      count === 1 ? 0 : -0.9 + (1.8 * lane) / (count - 1),
    );

  test.each(counts)("%i lanes each draw in their own colour", (count) => {
    const levels = spread(count);
    const colors = levels.map((_, lane) => laneColor(lane));
    const drawing = draw(flatLanes(levels), { laneColors: colors });
    expect(drawing.error).toBe(0);
    levels.forEach((level, lane) => {
      expectPixel(
        drawing,
        width / 2,
        rowOf(level),
        over(transparent, colors[lane] ?? laneColor(lane)),
        2,
      );
    });
  });

  test.each(counts)(
    "%i translucent lanes over each other layer in lane order",
    (count) => {
      const colors = Array.from({ length: count }, (_, lane) =>
        laneColor(lane, 0.5),
      );
      const drawing = draw(flatLanes(colors.map(() => 0)), {
        laneColors: colors,
      });
      expect(drawing.error).toBe(0);
      expectPixel(
        drawing,
        width / 2,
        rowOf(0),
        colors.reduce(over, transparent),
        count,
      );
    },
  );

  // Every lane but these pairs opaque at a level of its own; each pair shares one, across the
  // last channel of a target and the first of the next.
  test("a lane in the next target lies over the lane before it", () => {
    const pairs = Array.from(
      { length: Math.min(coverageAttachments, maxLanes / 4) - 1 },
      (_, target) => [4 * target + 3, 4 * target + 4] as const,
    );
    const seconds = new Set(pairs.map(([, second]) => second));
    let slot = -1;
    const slots = Array.from({ length: maxLanes }, (_, lane) =>
      seconds.has(lane) ? slot : ++slot,
    );
    const levels = slots.map((at) => -0.9 + (1.8 * at) / slot);
    const paired = new Set(pairs.flat());
    const colors = levels.map((_, lane) =>
      laneColor(lane, paired.has(lane) ? 0.5 : 1),
    );
    const drawing = draw(flatLanes(levels), { laneColors: colors });
    for (const [first, second] of pairs) {
      expectPixel(
        drawing,
        width / 2,
        rowOf(levels[first] ?? 0),
        over(
          over(transparent, colors[first] ?? laneColor(first)),
          colors[second] ?? laneColor(second),
        ),
        2,
      );
    }
  });

  // Two tracks that cancel in their sum, with the shading behind the sum lane; the traces
  // translucent over it, drawn twice, as the next frame is.
  test("translucent lanes over Compare's shading blend as before, frame after frame", () => {
    const own = { correlation: 0, related: false, offset: 0, inverted: false };
    const alike = {
      correlation: 0.9,
      related: true,
      offset: 0,
      inverted: false,
    };
    const count = 100;
    const band = (half: number) =>
      Array.from({ length: count * 2 }, (_, k) => (k % 2 === 0 ? -half : half));
    const frame = frameOf({
      mode: FrameMode.minMax,
      pointCount: count,
      viewStart: 0,
      viewEnd: 1,
      lanes: [band(0.5), band(0.4), band(0)],
      comparison: { sumGain: -3, fixedGain: 3, matches: [own, alike, alike] },
    });
    const colors = [0, 1, 2].map((lane) => laneColor(lane, 0.5));
    const options: DrawOptions = {
      laneColors: colors,
      comparedLanes: [true, true],
    };
    const shade: Rgba = [1, 0, 0, 0.16];
    const shaded = over(transparent, shade);
    const [first, second, sum] = colors as [Rgba, Rgba, Rgba];
    const places: [number, Pixel][] = [
      [height - 4, shaded],
      [rowOf(0.45), over(shaded, first)],
      [rowOf(0), over(over(over(shaded, first), second), sum)],
    ];
    for (const drawing of [draw(frame, options), draw(frame, options)]) {
      expect(drawing.error).toBe(0);
      for (const [y, expected] of places) {
        expectPixel(drawing, width / 2, y, expected, 3);
      }
    }
  });
});

describe("the coverage targets follow the canvas size", () => {
  // Every lane a diagonal across the whole screen, so every target holds some of the picture.
  const count = 60;
  const frame = frameOf({
    mode: FrameMode.minMax,
    pointCount: count,
    viewStart: 0,
    viewEnd: 1,
    lanes: Array.from({ length: maxLanes }, (_, lane) =>
      Array.from({ length: count }, (_, point) => {
        const value = -0.9 + (1.8 * point) / (count - 1) + lane * 0.01;
        return [value, value];
      }).flat(),
    ),
  });
  const laneColors = Array.from({ length: maxLanes }, (_, lane) =>
    laneColor(lane),
  );
  function expectSame(drawing: Drawing, reference: Drawing): void {
    expect(drawing.width).toBe(reference.width);
    for (let y = 0; y < drawing.height; y++) {
      for (let x = 0; x < drawing.width; x++) {
        expect(drawing.pixel(x, y), `${String(x)}, ${String(y)}`).toEqual(
          reference.pixel(x, y),
        );
      }
    }
  }

  test("grown, then hidden to no size and shown again, it draws as a renderer made at that size", () => {
    const surface = freshSurface(300, 150);
    drawOn(surface, frame, { laneColors });
    surface.canvas.width = 450;
    surface.canvas.height = 225;
    expectSame(
      drawOn(surface, frame, { laneColors }),
      draw(frame, { width: 450, height: 225, laneColors }),
    );
    surface.canvas.width = 0;
    surface.canvas.height = 0;
    expect(drawOn(surface, frame, { laneColors }).error).toBe(0);
    surface.canvas.width = 300;
    surface.canvas.height = 150;
    expectSame(
      drawOn(surface, frame, { laneColors }),
      draw(frame, { width: 300, height: 150, laneColors }),
    );
  });

  // A window made only taller, or only wider: the targets follow either.
  test.each([
    { name: "taller", width: 300, height: 240 },
    { name: "wider", width: 420, height: 150 },
  ])("made only $name, it draws as a renderer made at that size", (size) => {
    const surface = freshSurface(300, 150);
    drawOn(surface, frame, { laneColors });
    surface.canvas.width = size.width;
    surface.canvas.height = size.height;
    expectSame(
      drawOn(surface, frame, { laneColors }),
      draw(frame, { ...size, laneColors }),
    );
  });

  // A renderer made afresh, as after the graphics context comes back, draws its first frame on
  // the screen, not into its coverage targets: a frame with no points but a playhead.
  test("a new renderer's first frame reaches the screen", () => {
    const surface = freshSurface(300, 150);
    const empty = frameOf({
      mode: FrameMode.minMax,
      pointCount: 0,
      synced: true,
      cursor: 0.5,
    });
    const drawing = drawOn(surface, empty);
    expect(drawing.alpha(150, 75)).toBeGreaterThan(0.5);
  });
});
