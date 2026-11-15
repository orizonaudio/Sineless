// Where traces sit on the screen: span positions of a frame's points, the lanes of stacked views,
// the vertical scale, and the cancellation shading of Compare frames as clip-space triangles
// (x, y pairs; 6 per quad). Pure functions, unit-tested without a GPU; the traces themselves are
// built by the renderer's shaders on the same rules.
import {
  CompareLane,
  FrameMode,
  ViewLayout,
} from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import {
  cancellationBridgedPoints,
  cancellationDb,
  cancellationQuietShare,
  cancellationReachDivisor,
} from "../../../protocol/src/limits.ts";
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

// Raw point k sits k / pointsPerSample samples after the first sample, which is startFraction
// of a sample before the covered part's start.
export function pointPosition(frame: Frame, index: number): number {
  if (frame.visibleSamples <= 0) {
    return (
      frame.viewStart +
      (index / frame.pointCount) * (frame.viewEnd - frame.viewStart)
    );
  }
  return (
    frame.viewStart +
    ((index / frame.pointsPerSample - frame.startFraction) /
      frame.visibleSamples) *
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

// Each trace's coverage goes to one channel of an offscreen target: four channels a target, and
// four targets, so this many traces are layered over the screen in one pass: every frame's, since
// it is at least maxLanes.
export const coverageAttachments = 4;
export const tracesPerPass = coverageAttachments * 4;

// The targets `traces` traces use, from the first.
export function coverageTargets(traces: number): number {
  return Math.ceil(traces / 4);
}

// Where the trace at `index` writes its coverage.
export function coverageSlot(index: number): {
  readonly attachment: number;
  readonly channel: number;
} {
  return { attachment: Math.floor(index / 4), channel: index % 4 };
}

// The playhead's edges in clip space, at a position 0 to 1 across a screen widthPixels device
// pixels wide. A line straddling pixels is antialiased dimmer and wider by an amount that changes
// with its subpixel position, so a moving one flickers: its edges sit on whole device pixels, and
// it is one CSS pixel wide snapped as CSS snaps a border (at least one device pixel, else rounded
// down). At the screen's edges it stays centred on its position, and the screen clips it.
export function playheadEdges(
  position: number,
  widthPixels: number,
  pixelRatio: number,
): { readonly left: number; readonly right: number } {
  const width = Math.max(1, Math.floor(pixelRatio));
  const left = Math.round(position * widthPixels - width / 2);
  const clip = (pixels: number) => -1 + (2 * pixels) / widthPixels;
  return { left: clip(left), right: clip(left + width) };
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

// Compare frames hold the tracks shown and then their sum. Where the sum's peak falls
// cancellationDb below the louder track's, they cancel strongly: these spans of the sum's lane, as
// quads. Every lane before the sum's is a track summed. The rule and
// its numbers are in protocol/src/limits.ts; the macOS editor draws the same shading.
const cancellationRatio = 10 ** (cancellationDb / 20);
const quietRatio = cancellationQuietShare;
const bridgedGap = cancellationBridgedPoints;

// Each point's peak, NaN where a value is not finite (from a broken signal upstream).
// The level cancellationBands compares at each point of a trace (editor/src/TraceGeometry.cpp,
// cancellationLevels): in one pass, so its cost follows the points.
export function cancellationLevels(
  frame: Frame,
  values: Float32Array,
): Float32Array {
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
    frame.mode === FrameMode.minMax
      ? 0
      : Math.max(1, Math.floor(frame.pointCount / cancellationReachDivisor));
  // The largest finite level within `reach` points each side, in one pass: a queue of indices
  // whose levels fall from its front, so the front is the window's largest (a level that is not
  // finite counts as silence, which no level is below).
  const smoothed = new Float32Array(out.length);
  const level = (point: number): number => {
    const value = out[point] ?? 0;
    return Number.isFinite(value) ? value : 0;
  };
  const window = new Int32Array(out.length);
  let front = 0;
  let back = 0; // The queue is window[front, back).
  let next = 0; // The next point to enter the window.
  for (let point = 0; point < out.length; point++) {
    const to = Math.min(out.length - 1, point + reach);
    for (; next <= to; next++) {
      while (back > front && level(window[back - 1] ?? 0) <= level(next)) {
        back--;
      }
      window[back++] = next;
    }
    while ((window[front] ?? 0) + reach < point) {
      front++;
    }
    smoothed[point] = Number.isFinite(out[point])
      ? level(window[front] ?? 0)
      : Number.NaN;
  }
  return smoothed;
}

export function cancellationBands(
  frame: Frame,
  layout: TraceLayout,
): Float32Array {
  const sum = frame.lanes.at(-1);
  const tracks = frame.lanes.slice(0, -1);
  // Tracks that match this one nowhere clearly have no cancellation to fix: their sum only dips
  // and swells as they beat against each other. This track's own match is never related.
  const related =
    frame.comparison?.matches.some((match) => match.related) === true;
  // The difference is not a sum: where it falls below a track, the tracks agree, which is the
  // point of it, not a problem to mark.
  if (
    frame.comparison === undefined ||
    frame.comparison.lane === CompareLane.difference ||
    !sum ||
    tracks.length < 2 ||
    !related
  ) {
    return new Float32Array();
  }
  const levelSum = cancellationLevels(frame, sum);
  const louder = new Float32Array(frame.pointCount);
  for (const track of tracks) {
    cancellationLevels(frame, track).forEach((level, point) => {
      louder[point] = Math.max(louder[point] ?? 0, level);
    });
  }
  // A point where a track or the sum is not finite is measured around, as the engine measures
  // the gains: it cancels nothing, and a short gap it leaves is bridged below.
  const measured = (point: number) =>
    Number.isFinite(louder[point]) && Number.isFinite(levelSum[point]);
  // Quiet stretches (more than about 26 dB below the loudest moment) cancel nothing audible.
  const audible =
    quietRatio *
    Math.max(0, ...louder.filter((level) => Number.isFinite(level)));
  const cancelling = Array.from(
    louder,
    (level, point) =>
      measured(point) &&
      level > audible &&
      (levelSum[point] ?? 0) < cancellationRatio * level,
  );
  // Short gaps inside a cancelling stretch belong to it: of columns, or of samples, whatever the
  // points drawn between them.
  const bridged = bridgedGap * frame.pointsPerSample;
  for (let point = 1; point < cancelling.length; point++) {
    if (cancelling[point] === true || cancelling[point - 1] !== true) {
      continue;
    }
    const resume = cancelling.indexOf(true, point);
    if (resume > point && resume - point <= bridged) {
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

// A Compare frame showing the difference holds this track's lane, the other checked tracks'
// lanes (lined up by Align when it is on) and the difference itself. Its lane is not drawn as a
// wave about zero: each wave is taken as the area between it and the zero line, and the lane is
// filled with what this track's area and the others' share, what only this track's covers (lost)
// and what only the others' covers (gained), so that the difference shows where the amplitude
// went. At every point the lost and gained heights add up to the difference there.
//
// Each part is a block of the trace renderer's own kind, a low and a high per point (NaN where
// the part is absent), drawn as the lanes' columns are: what is shared; what is lost above it and
// below it; what is gained above it and below it. A part absent beside a point where it is
// present ends on the shared edge there, at no height, so it tapers to its neighbour rather than
// stopping a point short. The macOS editor draws the same blocks (editor/src/TraceGeometry.cpp).
export const differenceParts = [
  "shared",
  "lostAbove",
  "lostBelow",
  "gainedAbove",
  "gainedBelow",
] as const;
export type DifferencePart = (typeof differenceParts)[number];

// The blocks, in differenceParts' order, each of pointCount (low, high) pairs; none for a frame
// that does not show a difference. Every lane before the difference's is a track compared; `own`
// is this track's lane.
export function differenceAreas(
  frame: Frame,
  own: number,
): readonly Float32Array[] | undefined {
  const difference = frame.lanes.at(-1);
  const mine = frame.lanes[own];
  const others = frame.lanes.slice(0, -1).filter((_, index) => index !== own);
  if (
    frame.comparison?.lane !== CompareLane.difference ||
    difference === undefined ||
    mine === undefined ||
    others.length === 0
  ) {
    return undefined;
  }
  const count = frame.pointCount;
  const all = new Float32Array(differenceParts.length * count * 2).fill(
    Number.NaN,
  );
  const blocks = differenceParts.map((_, part) =>
    all.subarray(part * count * 2, (part + 1) * count * 2),
  );
  const [shared, lostAbove, lostBelow, gainedAbove, gainedBelow] = blocks as [
    Float32Array,
    Float32Array,
    Float32Array,
    Float32Array,
    Float32Array,
  ];
  const columns = frame.mode === FrameMode.minMax;
  for (let point = 0; point < count; point++) {
    // How far each wave reaches from zero at this point: a column's lowest and highest sample,
    // or a raw point's value. The others' wave is their columns added (at least as far as their
    // sum reaches), or, point by point, this track's less the difference, exactly.
    let low: number;
    let high: number;
    let otherLow = 0;
    let otherHigh = 0;
    if (columns) {
      low = mine[point * 2] ?? 0;
      high = mine[point * 2 + 1] ?? 0;
      for (const other of others) {
        otherLow += other[point * 2] ?? 0;
        otherHigh += other[point * 2 + 1] ?? 0;
      }
    } else {
      low = mine[point] ?? 0;
      high = low;
      otherLow = low - (difference[point] ?? 0);
      otherHigh = otherLow;
    }
    if (![low, high, otherLow, otherHigh].every(Number.isFinite)) {
      continue;
    }
    low = Math.min(0, low);
    high = Math.max(0, high);
    otherLow = Math.min(0, otherLow);
    otherHigh = Math.max(0, otherHigh);
    const sharedLow = Math.max(low, otherLow);
    const sharedHigh = Math.min(high, otherHigh);
    shared[point * 2] = sharedLow;
    shared[point * 2 + 1] = sharedHigh;
    if (otherHigh !== high) {
      const above = otherHigh > high ? gainedAbove : lostAbove;
      above[point * 2] = sharedHigh;
      above[point * 2 + 1] = Math.max(high, otherHigh);
    }
    if (otherLow !== low) {
      const below = otherLow < low ? gainedBelow : lostBelow;
      below[point * 2] = Math.min(low, otherLow);
      below[point * 2 + 1] = sharedLow;
    }
  }
  // The tapers: a part absent at a point with a present neighbour ends on the shared edge.
  const taper = (block: Float32Array, edge: 0 | 1) => {
    const present = (point: number) =>
      point >= 0 &&
      point < count &&
      Number.isFinite(block[point * 2] ?? Number.NaN) &&
      block[point * 2] !== block[point * 2 + 1];
    for (let point = 0; point < count; point++) {
      const at = shared[point * 2 + edge] ?? Number.NaN;
      if (
        Number.isNaN(block[point * 2] ?? Number.NaN) &&
        Number.isFinite(at) &&
        (present(point - 1) || present(point + 1))
      ) {
        block[point * 2] = at;
        block[point * 2 + 1] = at;
      }
    }
  };
  taper(lostAbove, 1);
  taper(gainedAbove, 1);
  taper(lostBelow, 0);
  taper(gainedBelow, 0);
  return blocks;
}
