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

export function traceGeometry(
  frame: Frame,
  layout: TraceLayout,
): Map<ChannelView, Float32Array> {
  const views = [...frame.views.entries()];
  const viewLanes = lanes(views.length, layout.layout);
  const scale = fullScale(layout.verticalScaleDb);
  const geometry = new Map<ChannelView, Float32Array>();
  views.forEach(([view, values], index) => {
    const lane = viewLanes[index] ?? { center: 0, halfHeight: 1 };
    geometry.set(
      view,
      frame.mode === FrameMode.minMax
        ? minMaxBand(values, frame, lane, scale, layout)
        : rawLine(values, frame, lane, scale, layout),
    );
  });
  return geometry;
}
