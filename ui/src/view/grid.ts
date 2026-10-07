// The screen grid: 10 time divisions, level lines every 3 dB down from the top of each lane and
// a brighter zero line per lane, drawn on a 2D canvas behind the traces. Redrawn only when the
// size or the lanes change; level lines do not depend on the vertical scale.
import { ViewLayout } from "../../../protocol/src/enums.ts";
import { lanes, type Lane } from "../render/trace-geometry.ts";
import {
  gridColumns,
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
  context.lineWidth = Math.max(1, Math.round(window.devicePixelRatio));
  const line = (x0: number, y0: number, x1: number, y1: number) => {
    context.beginPath();
    context.moveTo(x0, y0);
    context.lineTo(x1, y1);
    context.stroke();
  };
  const columns =
    verticalLines ??
    Array.from({ length: gridColumns - 1 }, (_, index) => ({
      position: (index + 1) / gridColumns,
      strong: false,
    }));
  for (const column of columns) {
    context.strokeStyle = column.strong ? colors.axis : colors.grid;
    const x = Math.round(width * column.position) + 0.5;
    line(x, 0, x, height);
  }
  const laneList = lanes(laneCount, ViewLayout.stacked);
  const rowY = (lane: Lane, fraction: number) =>
    Math.round(
      ((1 - (lane.center + fraction * lane.halfHeight)) / 2) * height,
    ) + 0.5;
  context.strokeStyle = colors.grid;
  for (const lane of laneList) {
    const halfLaneCss =
      (lane.halfHeight * height) / 2 / Math.max(1, window.devicePixelRatio);
    for (const step of levelSteps(halfLaneCss, minimumLineGap)) {
      for (const sign of [1, -1]) {
        const y = rowY(lane, sign * levelFraction(step));
        line(0, y, width, y);
      }
    }
  }
  context.strokeStyle = colors.axis;
  for (const lane of laneList) {
    const y = rowY(lane, 0);
    line(0, y, width, y);
  }
}
