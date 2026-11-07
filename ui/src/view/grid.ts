// The screen grid, by the grid settings of the time base shown: equal time divisions (or Beats'
// lines), level lines down from the top of each lane and a brighter zero line per lane (Grid); the
// zero lines alone (None); or the zero lines and the trigger point's vertical line, with ticks where
// the time and level lines would be (Cross hair). Drawn on a 2D canvas behind the traces. Redrawn only when the size, the lanes or the grid settings change;
// level lines do not depend on the vertical scale. The macOS editor draws the same lines natively
// (gridLines; protocol/tests/data/trace-geometry.json holds cases both read).
import { ViewLayout } from "../../../protocol/src/enums.ts";
import {
  gridColumnsCoarse,
  gridColumnsFine,
  gridColumnsMedium,
  gridTickLength,
} from "../../../protocol/src/limits.ts";
import { lanes, type Lane } from "../render/trace-geometry.ts";
import { dbFraction, levelLines } from "./axis-labels.ts";
import type { GridDensityName, GridTypeName } from "./grid-settings.ts";

// Vertical lines at the given positions (fractions of the width), e.g. on beats in a tempo-synced
// sweep; strong lines use the axis colour. Without them, the Time step's equal divisions are drawn.
export interface VerticalLine {
  readonly position: number;
  readonly strong: boolean;
}

// The grid's lines in device pixels: each a line `lineWidth` wide centred at `x` (vertical) or
// `y` (horizontal) from the top left; `strong` and `axis` lines take the axis colour (the zero
// lines, and Fine's 3 dB lines). Ticks are short lines as wide, from (x0, y0) to (x1, y1), either
// vertical (x0 = x1) or horizontal (y0 = y1), strong ones in the axis colour.
export interface GridTick {
  readonly x0: number;
  readonly y0: number;
  readonly x1: number;
  readonly y1: number;
  readonly strong: boolean;
}

export interface GridLines {
  readonly lineWidth: number;
  readonly vertical: readonly {
    readonly x: number;
    readonly strong: boolean;
  }[];
  readonly horizontal: readonly {
    readonly y: number;
    readonly axis: boolean;
  }[];
  readonly ticks: readonly GridTick[];
}

// The equal divisions of the width at each Time step.
export const gridColumns: Readonly<Record<GridDensityName, number>> = {
  coarse: gridColumnsCoarse,
  medium: gridColumnsMedium,
  fine: gridColumnsFine,
};

// What to draw: the type (as drawn: Cross hair only in Trig), the Level step, the number of equal
// time divisions (gridColumns by the Time step), Beats' lines in their place, and for Cross hair
// the trigger point's position on screen (0 to 1; elsewhere it is off screen).
export interface GridOptions {
  readonly type?: GridTypeName;
  readonly level?: GridDensityName;
  readonly columns?: number;
  readonly verticalLines?: readonly VerticalLine[] | undefined;
  readonly trigger?: number;
}

export function gridLines(
  width: number,
  height: number,
  laneCount: number,
  pixelRatio: number,
  options: GridOptions = {},
): GridLines {
  const type = options.type ?? "grid";
  const count = options.columns ?? gridColumnsMedium;
  const divisions =
    options.verticalLines ??
    Array.from({ length: count - 1 }, (_, index) => ({
      position: (index + 1) / count,
      strong: false,
    }));
  const xOf = (position: number) => Math.round(width * position) + 0.5;
  const laneList = lanes(laneCount, ViewLayout.stacked);
  const rowY = (lane: Lane, fraction: number) =>
    Math.round(
      ((1 - (lane.center + fraction * lane.halfHeight)) / 2) * height,
    ) + 0.5;
  // Each lane's level lines, both signs, top lane first.
  const levelRows = laneList.flatMap((lane) => {
    const halfLaneCss =
      (lane.halfHeight * height) / 2 / Math.max(1, pixelRatio);
    return levelLines(halfLaneCss, options.level ?? "medium").flatMap((line) =>
      [1, -1].map((sign) => ({
        y: rowY(lane, sign * dbFraction(line.step)),
        axis: line.strong,
      })),
    );
  });
  const zeroRows = laneList.map((lane) => ({ y: rowY(lane, 0), axis: true }));

  const vertical: { x: number; strong: boolean }[] = [];
  const horizontal: { y: number; axis: boolean }[] = [];
  const ticks: GridTick[] = [];
  if (type === "grid") {
    for (const column of divisions) {
      vertical.push({ x: xOf(column.position), strong: column.strong });
    }
    horizontal.push(...levelRows);
  } else if (type === "crossHair") {
    const half = (gridTickLength * Math.max(1, pixelRatio)) / 2;
    for (const zero of zeroRows) {
      for (const column of divisions) {
        const x = xOf(column.position);
        ticks.push({
          x0: x,
          y0: zero.y - half,
          x1: x,
          y1: zero.y + half,
          strong: column.strong,
        });
      }
    }
    const trigger = options.trigger;
    if (trigger !== undefined && trigger >= 0 && trigger <= 1) {
      const x = xOf(trigger);
      vertical.push({ x, strong: true });
      for (const row of levelRows) {
        ticks.push({
          x0: x - half,
          y0: row.y,
          x1: x + half,
          y1: row.y,
          strong: row.axis,
        });
      }
    }
  }
  horizontal.push(...zeroRows);
  return {
    lineWidth: Math.max(1, Math.round(pixelRatio)),
    vertical,
    horizontal,
    ticks,
  };
}

export function drawGrid(
  canvas: HTMLCanvasElement,
  laneCount: number,
  colors: { grid: string; axis: string },
  options: GridOptions = {},
): void {
  const context = canvas.getContext("2d");
  if (context === null) {
    return;
  }
  const { width, height } = canvas;
  context.clearRect(0, 0, width, height);
  const lines = gridLines(
    width,
    height,
    laneCount,
    window.devicePixelRatio,
    options,
  );
  context.lineWidth = lines.lineWidth;
  const line = (x0: number, y0: number, x1: number, y1: number) => {
    context.beginPath();
    context.moveTo(x0, y0);
    context.lineTo(x1, y1);
    context.stroke();
  };
  // Ticks first, so the zero and cross lines they mark stay whole over them.
  for (const tick of lines.ticks) {
    context.strokeStyle = tick.strong ? colors.axis : colors.grid;
    line(tick.x0, tick.y0, tick.x1, tick.y1);
  }
  for (const column of lines.vertical) {
    context.strokeStyle = column.strong ? colors.axis : colors.grid;
    line(column.x, 0, column.x, height);
  }
  // Level lines, then each lane's zero line over them.
  for (const row of lines.horizontal) {
    context.strokeStyle = row.axis ? colors.axis : colors.grid;
    line(0, row.y, width, row.y);
  }
}
