// Turns a frame into clip-space triangles, one array per view (x, y pairs; 6 per quad). Pure
// functions, so the geometry is unit-tested without a GPU.
import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { fullView, type ViewWindow } from "../view/zoom.ts";

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

// One quad per column from min to max, at least one line width tall. A column lying wholly
// above or below its predecessor is extended to meet it: the signal passes through that gap
// between the two columns' samples, and without the join, columns holding only one or two
// samples would show as separate dots.
function minMaxBand(
  values: Float32Array,
  frame: Frame,
  lane: Lane,
  scale: number,
  layout: TraceLayout,
): Float32Array {
  const out: number[] = [];
  const view = layout.view ?? fullView;
  const minimumHalf = layout.lineWidthPixels / layout.heightPixels;
  let previousLow = Number.NaN;
  let previousHigh = Number.NaN;
  for (let column = 0; column < frame.pointCount; column++) {
    let low =
      lane.center + ((values[column * 2] ?? 0) / scale) * lane.halfHeight;
    let high =
      lane.center + ((values[column * 2 + 1] ?? 0) / scale) * lane.halfHeight;
    const sampleLow = low;
    const sampleHigh = high;
    if (low > previousHigh) {
      low = previousHigh;
    } else if (high < previousLow) {
      high = previousLow;
    }
    previousLow = sampleLow;
    previousHigh = sampleHigh;
    if (high - low < 2 * minimumHalf) {
      const middle = (low + high) / 2;
      low = middle - minimumHalf;
      high = middle + minimumHalf;
    }
    const left = screenX(columnPosition(frame, column), view);
    const right = screenX(columnPosition(frame, column + 1), view);
    pushQuad(out, left, low, left, high, right, low, right, high);
  }
  return new Float32Array(out);
}

// A polyline through the samples, one quad per segment, `lineWidthPixels` thick.
function rawLine(
  values: Float32Array,
  frame: Frame,
  lane: Lane,
  scale: number,
  layout: TraceLayout,
): Float32Array {
  const out: number[] = [];
  const toPixelsX = layout.widthPixels / 2;
  const toPixelsY = layout.heightPixels / 2;
  const halfWidth = layout.lineWidthPixels / 2;
  const view = layout.view ?? fullView;
  const x = (index: number) => screenX(pointPosition(frame, index), view);
  const y = (index: number) =>
    lane.center + ((values[index] ?? 0) / scale) * lane.halfHeight;
  for (let index = 0; index + 1 < frame.pointCount; index++) {
    const x0 = x(index);
    const y0 = y(index);
    const x1 = x(index + 1);
    const y1 = y(index + 1);
    // Offset perpendicular to the segment, computed in pixels so the width is even.
    const dx = (x1 - x0) * toPixelsX;
    const dy = (y1 - y0) * toPixelsY;
    const length = Math.hypot(dx, dy) || 1;
    const offsetX = ((-dy / length) * halfWidth) / toPixelsX;
    const offsetY = ((dx / length) * halfWidth) / toPixelsY;
    pushQuad(
      out,
      x0 - offsetX,
      y0 - offsetY,
      x0 + offsetX,
      y0 + offsetY,
      x1 - offsetX,
      y1 - offsetY,
      x1 + offsetX,
      y1 + offsetY,
    );
  }
  return new Float32Array(out);
}

// One array per block of points (a view or a lane), each in its lane.
function blockGeometry(
  blocks: readonly Float32Array[],
  frame: Frame,
  layout: TraceLayout,
): Float32Array[] {
  const blockLanes = lanes(blocks.length, layout.layout);
  const scale = fullScale(layout.verticalScaleDb);
  return blocks.map((values, index) => {
    const lane = blockLanes[index] ?? { center: 0, halfHeight: 1 };
    return frame.mode === FrameMode.minMax
      ? minMaxBand(values, frame, lane, scale, layout)
      : rawLine(values, frame, lane, scale, layout);
  });
}

export function traceGeometry(
  frame: Frame,
  layout: TraceLayout,
): Map<ChannelView, Float32Array> {
  const views = [...frame.views.entries()];
  const geometry = blockGeometry(
    views.map(([, values]) => values),
    frame,
    layout,
  );
  return new Map(
    views.map(([view], index) => [view, geometry[index] ?? new Float32Array()]),
  );
}

// Multi-track frames: one array per lane, in lane order.
export function laneGeometry(
  frame: Frame,
  layout: TraceLayout,
): Float32Array[] {
  return blockGeometry(frame.lanes, frame, layout);
}

// Compare frames hold the group's tracks and then the sum of the compared ones. Where the sum's
// peak level falls more than 3 dB below the loudest compared track, they cancel: these spans of
// the sum's lane, as quads. `compared` marks the compared tracks in lane order.
const cancellationRatio = 0.708; // -3 dB.
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
  const louder = new Float32Array(frame.pointCount);
  for (const track of tracks) {
    peaks(frame, track).forEach((level, point) => {
      louder[point] = Math.max(louder[point] ?? 0, level);
    });
  }
  // Quiet stretches (more than about 26 dB below the loudest moment) cancel nothing audible.
  const audible = quietRatio * Math.max(0, ...louder);
  const cancelling = Array.from(
    louder,
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
