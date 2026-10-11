// Where traces sit on the screen: span positions of a frame's points, the lanes of stacked views,
// the vertical scale, and the cancellation shading of Compare frames as clip-space triangles
// (x, y pairs; 6 per quad). Pure functions, unit-tested without a GPU; the traces themselves are
// built by the renderer's shaders on the same rules.
import { FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { fullView, type ViewWindow } from "../view/zoom.ts";
import { summingMarginDb } from "../../../protocol/src/limits.ts";

export interface TraceLayout {
  readonly widthPixels: number; // Device pixels.
  readonly heightPixels: number;
  readonly verticalScaleDb: number; // dBFS at the top of the screen.
  readonly layout: ViewLayout;
  readonly lineWidthPixels: number;
  // The part of the frame on screen; the whole frame when absent.
  readonly view?: ViewWindow;
}

// Clip-space x of a frame position (0 to 1) under the view zoom.
// Span position (0 to 1) of a min/max column edge or a raw point. A frame covers the part
// viewStart to viewEnd of the span; one rendered for an older view is thereby stretched
// into place until a frame for the current view arrives.
export function columnPosition(frame: Frame, column: number): number {
  return (
    frame.viewStart +
    (column / frame.pointCount) * (frame.viewEnd - frame.viewStart)
  );
}

export function pointPosition(frame: Frame, index: number): number {
  const samples =
    frame.visibleSamples > 0 ? frame.visibleSamples : frame.pointCount;
  return (
    frame.viewStart +
    ((index - frame.startFraction) / samples) *
      (frame.viewEnd - frame.viewStart)
  );
}

function screenX(frameFraction: number, view: ViewWindow): number {
  return -1 + (2 * (frameFraction - view.start)) / (view.end - view.start);
}

// Vertical placement of one view: overlaid views share the screen, stacked views get a lane each.
export interface Lane {
  readonly center: number; // Clip space.
  readonly halfHeight: number;
}

export function lanes(count: number, layout: ViewLayout): Lane[] {
  if (layout === ViewLayout.overlaid || count <= 1) {
    return Array.from({ length: count }, () => ({ center: 0, halfHeight: 1 }));
  }
  const halfHeight = 1 / count;
  return Array.from({ length: count }, (_, index) => ({
    center: 1 - halfHeight * (2 * index + 1),
    halfHeight,
  }));
}

export function fullScale(verticalScaleDb: number): number {
  return 10 ** (verticalScaleDb / 20);
}

function pushQuad(
  out: number[],
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  x3: number,
  y3: number,
): void {
  // Two triangles: (0, 1, 2) and (2, 1, 3).
  out.push(x0, y0, x1, y1, x2, y2, x2, y2, x1, y1, x3, y3);
}

// Compare frames hold the group's tracks and then the sum of the compared ones. Where the sum's
// peak level falls more than summingMarginDb below the root-sum-square of the compared tracks'
// peaks (what unrelated tracks would sum to), they cancel: these spans of the sum's lane, as
// quads. `compared` marks the compared tracks in lane order.
const cancellationRatio = 10 ** (-summingMarginDb / 20);
const quietRatio = 0.05; // About -26 dB.
const bridgedGap = 3; // Points.

function peaks(frame: Frame, values: Float32Array): Float32Array {
  const out = new Float32Array(frame.pointCount);
  for (let point = 0; point < frame.pointCount; point++) {
    out[point] =
      frame.mode === FrameMode.minMax
        ? Math.max(
            Math.abs(values[point * 2] ?? 0),
            Math.abs(values[point * 2 + 1] ?? 0),
          )
        : Math.abs(values[point] ?? 0);
  }
  // Min/max columns already hold their peaks; raw points cross zero every cycle, so their levels
  // are compared over a short window.
  const reach =
    frame.mode === FrameMode.minMax ? 0 : Math.max(1, frame.pointCount >> 7);
  const smoothed = new Float32Array(out.length);
  for (let point = 0; point < out.length; point++) {
    let peak = 0;
    for (
      let near = Math.max(0, point - reach);
      near <= Math.min(out.length - 1, point + reach);
      near++
    ) {
      peak = Math.max(peak, out[near] ?? 0);
    }
    smoothed[point] = peak;
  }
  return smoothed;
}

export function cancellationBands(
  frame: Frame,
  layout: TraceLayout,
  compared: readonly boolean[],
): Float32Array {
  const sum = frame.lanes.at(-1);
  const tracks = frame.lanes
    .slice(0, -1)
    .filter((_, index) => compared[index] === true);
  if (frame.comparison === undefined || !sum || tracks.length < 2) {
    return new Float32Array();
  }
  const levelSum = peaks(frame, sum);
  const unrelated = new Float32Array(frame.pointCount);
  for (const track of tracks) {
    peaks(frame, track).forEach((level, point) => {
      unrelated[point] = (unrelated[point] ?? 0) + level * level;
    });
  }
  unrelated.forEach((power, point) => {
    unrelated[point] = Math.sqrt(power);
  });
  // Quiet stretches (more than about 26 dB below the loudest moment) cancel nothing audible.
  const audible = quietRatio * Math.max(0, ...unrelated);
  const cancelling = Array.from(
    unrelated,
    (level, point) =>
      level > audible && (levelSum[point] ?? 0) < cancellationRatio * level,
  );
  // Short gaps inside a cancelling stretch belong to it.
  for (let point = 1; point < cancelling.length; point++) {
    if (cancelling[point] === true || cancelling[point - 1] !== true) {
      continue;
    }
    const resume = cancelling.indexOf(true, point);
    if (resume > point && resume - point <= bridgedGap) {
      cancelling.fill(true, point, resume);
    }
  }
  const count = frame.lanes.length;
  const lane = lanes(count, layout.layout)[count - 1] ?? {
    center: 0,
    halfHeight: 1,
  };
  const view = layout.view ?? fullView;
  const position = (point: number) =>
    frame.mode === FrameMode.minMax
      ? columnPosition(frame, point)
      : pointPosition(frame, point);
  const out: number[] = [];
  const bottom = lane.center - lane.halfHeight;
  const top = lane.center + lane.halfHeight;
  let start = -1;
  for (let point = 0; point <= cancelling.length; point++) {
    const here = cancelling[point] === true;
    if (here && start < 0) {
      start = point;
    } else if (!here && start >= 0) {
      const x0 = screenX(position(start), view);
      const x1 = screenX(position(point), view);
      pushQuad(out, x0, bottom, x1, bottom, x0, top, x1, top);
      start = -1;
    }
  }
  return new Float32Array(out);
}
