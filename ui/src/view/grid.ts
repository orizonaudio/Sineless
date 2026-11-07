// The screen grid: 10 time divisions, level lines every 3 dB down from the top of each lane and
// a brighter zero line per lane, drawn on a 2D canvas behind the traces. Redrawn only when the
// size or the lanes change; level lines do not depend on the vertical scale. The macOS editor
// draws the same lines natively (gridLines; protocol/tests/data/trace-geometry.json holds cases
// both read).
import { ViewLayout } from "../../../protocol/src/enums.ts";
import { lanes, type Lane } from "../render/trace-geometry.ts";
import {
  gridColumnsMedium,
  levelFraction,
  levelSteps,
  minimumLineGap,
} from "./axis-labels.ts";

// Vertical lines at the given positions (fractions of the width), e.g. on beats in a tempo-synced
// sweep; strong lines use the axis colour. Without them, the 10 equal divisions are drawn.
export interface VerticalLine {
  readonly position: number;
  readonly strong: boolean;
}

// The grid's lines in device pixels: each a line `lineWidth` wide centred at `x` (vertical) or
// `y` (horizontal) from the top left; `strong` and `axis` lines take the axis colour.
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
}

export function gridLines(
  width: number,
  height: number,
  laneCount: number,
  pixelRatio: number,
  verticalLines?: readonly VerticalLine[],
): GridLines {
  const columns =
    verticalLines ??
    Array.from({ length: gridColumnsMedium - 1 }, (_, index) => ({
      position: (index + 1) / gridColumnsMedium,
      strong: false,
    }));
  const vertical = columns.map((column) => ({
    x: Math.round(width * column.position) + 0.5,
    strong: column.strong,
  }));
  const laneList = lanes(laneCount, ViewLayout.stacked);
  const rowY = (lane: Lane, fraction: number) =>
    Math.round(
      ((1 - (lane.center + fraction * lane.halfHeight)) / 2) * height,
    ) + 0.5;
  const horizontal: { y: number; axis: boolean }[] = [];
  for (const lane of laneList) {
    const halfLaneCss =
      (lane.halfHeight * height) / 2 / Math.max(1, pixelRatio);
    for (const step of levelSteps(halfLaneCss, minimumLineGap)) {
      for (const sign of [1, -1]) {
        horizontal.push({
          y: rowY(lane, sign * levelFraction(step)),
          axis: false,
        });
      }
    }
  }
  for (const lane of laneList) {
    horizontal.push({ y: rowY(lane, 0), axis: true });
  }
  return {
    lineWidth: Math.max(1, Math.round(pixelRatio)),
    vertical,
    horizontal,
  };
}

export function drawGrid(
  canvas: HTMLCanvasElement,
  laneCount: number,
  colors: { grid: string; axis: string },
  verticalLines?: readonly VerticalLine[],
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
    verticalLines,
  );
  context.lineWidth = lines.lineWidth;
  const line = (x0: number, y0: number, x1: number, y1: number) => {
    context.beginPath();
    context.moveTo(x0, y0);
    context.lineTo(x1, y1);
    context.stroke();
  };
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
