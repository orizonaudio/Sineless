// The hover readout: what lies under the pointer on the screen. It names the trace, gives the
// position (bar.beat.sixteenth and project time in Beats mode, time from the trigger point
// otherwise), the trace's peak level there, and the length of the cycle around the pointer from
// its rising crossings of its mean, with that cycle's frequency and nearest note. It also says where it
// read: the column or point under the pointer, and the cycle's extent, for the screen to mark.
import { FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import {
  crossingHysteresis,
  periodTolerance,
  analysisGateDbfs,
} from "../../../protocol/src/limits.ts";
import {
  columnPosition,
  lanes,
  pointPosition,
} from "../render/trace-geometry.ts";
import { triggerPosition } from "./axis-labels.ts";
import { noteName } from "./cursors.ts";
import {
  formatClock,
  formatDbfs,
  formatFrequency,
  formatSamples,
  formatSeconds,
} from "./format.ts";
import { musicalPosition } from "./musical-grid.ts";
import { toFrame, toScreen, type ViewWindow } from "./zoom.ts";

// An RMS level at the analysis gate: quieter signals have no cycles, as in the engine.
const gateLevel = 10 ** (analysisGateDbfs / 20);

export interface HoverTrace {
  readonly name: string;
  readonly values: Float32Array;
}

export interface HoverContext {
  readonly frame: Frame;
  // The traces in lane order as drawn; in an overlay, `overlaid` picks the one described.
  readonly traces: readonly HoverTrace[];
  readonly overlaid: number;
  readonly layout: ViewLayout;
  readonly view: ViewWindow;
  readonly a4: number;
  // Seconds mode: the span rolls, so times count back from now.
  readonly freeRunning: boolean;
}

// A cycle needs this many min/max columns to be measured from them: each crossing is only known
// to within a column, so fewer would err by more than about 8%. Even then the length is rounded
// and the note given without cents.
const minimumColumnsPerCycle = 24;

// The trace under `y` (0 at the top of the screen, 1 at the bottom): the stacked lane there, or
// the overlaid trace.
export function traceAt(
  count: number,
  layout: ViewLayout,
  y: number,
  overlaid: number,
): number {
  if (layout !== ViewLayout.stacked || count <= 1) {
    return Math.min(Math.max(overlaid, 0), Math.max(count - 1, 0));
  }
  const clip = 1 - 2 * y;
  const found = lanes(count, layout).findIndex(
    (lane) => Math.abs(clip - lane.center) <= lane.halfHeight,
  );
  return found < 0 ? (clip > 0 ? 0 : count - 1) : found;
}

// Span positions (0 to 1) where `values` rises through its mean level, by the engine's rule for
// its frequency readout: only after first falling `crossingHysteresis` of the peak-to-peak range
// below it, so noise riding on a crossing does not count twice. None for a signal at or below
// the analysis gate. Min/max columns cross inside a column that spans the level, entered from
// below, or at the edge between a column wholly below it and one wholly above. Values that are
// not finite (from a broken signal upstream) are measured around, as the engine does.
export function risingCrossings(frame: Frame, values: Float32Array): number[] {
  const crossings: number[] = [];
  const count =
    frame.mode === FrameMode.minMax ? frame.pointCount * 2 : frame.pointCount;
  let lowest = Infinity;
  let highest = -Infinity;
  let total = 0;
  let squares = 0;
  let finite = 0;
  for (let index = 0; index < count; index++) {
    const value = values[index] ?? 0;
    if (!Number.isFinite(value)) {
      continue;
    }
    lowest = Math.min(lowest, value);
    highest = Math.max(highest, value);
    total += value;
    squares += value * value;
    finite++;
  }
  const range = highest - lowest;
  if (
    finite === 0 ||
    !(Math.sqrt(squares / finite) > gateLevel) ||
    !(range > 0)
  ) {
    return crossings;
  }
  const level = total / finite;
  const armLevel = level - crossingHysteresis * range;
  let armed = false;
  if (frame.mode === FrameMode.minMax) {
    for (let column = 0; column < frame.pointCount; column++) {
      const low = values[column * 2] ?? 0;
      const high = values[column * 2 + 1] ?? 0;
      if (!Number.isFinite(low) || !Number.isFinite(high)) {
        continue;
      }
      if (armed && high >= level) {
        crossings.push(
          columnPosition(frame, low >= level ? column : column + 0.5),
        );
        armed = false;
      } else if (low <= armLevel) {
        armed = true;
      }
    }
    return crossings;
  }
  for (let index = 0; index + 1 < frame.pointCount; index++) {
    const here = values[index] ?? 0;
    const next = values[index + 1] ?? 0;
    if (!Number.isFinite(here)) {
      continue;
    }
    if (here <= armLevel) {
      armed = true;
    }
    if (armed && Number.isFinite(next) && here < level && next >= level) {
      crossings.push(
        pointPosition(frame, index + (level - here) / (next - here)),
      );
      armed = false;
    }
  }
  return crossings;
}

// The peak level of `values` at span position `position`: the column's larger magnitude, or the
// magnitude of the nearest raw point.
export function levelAt(
  frame: Frame,
  values: Float32Array,
  position: number,
): number | undefined {
  const covered = frame.viewEnd - frame.viewStart;
  if (frame.pointCount === 0 || covered <= 0) {
    return undefined;
  }
  const along = (position - frame.viewStart) / covered;
  if (frame.mode === FrameMode.minMax) {
    const column = Math.floor(along * frame.pointCount);
    if (column < 0 || column >= frame.pointCount) {
      return undefined;
    }
    return Math.max(
      Math.abs(values[column * 2] ?? 0),
      Math.abs(values[column * 2 + 1] ?? 0),
    );
  }
  const index = Math.round(frame.startFraction + along * frame.visibleSamples);
  if (index < 0 || index >= frame.pointCount) {
    return undefined;
  }
  return Math.abs(values[index] ?? 0);
}

// The span position actually read for `position`: the centre of its min/max column, or the
// nearest raw point. Undefined off the frame.
export function readPosition(
  frame: Frame,
  position: number,
): number | undefined {
  const covered = frame.viewEnd - frame.viewStart;
  if (frame.pointCount === 0 || covered <= 0) {
    return undefined;
  }
  const along = (position - frame.viewStart) / covered;
  if (frame.mode === FrameMode.minMax) {
    const column = Math.floor(along * frame.pointCount);
    if (column < 0 || column >= frame.pointCount) {
      return undefined;
    }
    return columnPosition(frame, column + 0.5);
  }
  const index = Math.round(frame.startFraction + along * frame.visibleSamples);
  if (index < 0 || index >= frame.pointCount) {
    return undefined;
  }
  return pointPosition(frame, index);
}

// The cycle around `position`, as fractions of the span: from the last rising crossing at or
// before it to the next one after it. Undefined when either is missing, or when min/max columns
// are too coarse to tell.
export function cycleBounds(
  frame: Frame,
  values: Float32Array,
  position: number,
): { start: number; end: number } | undefined {
  const crossings = risingCrossings(frame, values);
  const next = crossings.findIndex((crossing) => crossing > position);
  const after = crossings[next];
  const before = next > 0 ? crossings[next - 1] : undefined;
  if (after === undefined || before === undefined) {
    return undefined;
  }
  // A cycle is only a cycle if the signal repeats: next to another of about the same length,
  // as the engine's frequency readout requires. A lone cycle, zoomed in on, has none to compare.
  const length = after - before;
  const neighbours = [
    next > 1 ? before - (crossings[next - 2] ?? before) : undefined,
    next + 1 < crossings.length
      ? (crossings[next + 1] ?? after) - after
      : undefined,
  ].filter((other) => other !== undefined);
  if (
    neighbours.length > 0 &&
    !neighbours.some(
      (other) => Math.abs(other - length) <= periodTolerance * length,
    )
  ) {
    return undefined;
  }
  if (frame.mode === FrameMode.minMax) {
    const columnWidth = (frame.viewEnd - frame.viewStart) / frame.pointCount;
    if (after - before < minimumColumnsPerCycle * columnWidth) {
      return undefined;
    }
  }
  return { start: before, end: after };
}

// The length of that cycle, as a fraction of the span.
export function cycleAt(
  frame: Frame,
  values: Float32Array,
  position: number,
): number | undefined {
  const bounds = cycleBounds(frame, values, position);
  return bounds === undefined ? undefined : bounds.end - bounds.start;
}

// What the hover readout shows: a title and lines, and where it read, as screen positions
// (0 to 1, clipped to the screen): the column or point, the cycle measured, if any, and the
// trace's lane from `top` to `bottom` (the whole screen unless lanes are stacked).
export interface HoverReadout {
  readonly title: string;
  readonly lines: string[];
  readonly x: number;
  readonly cycle: { start: number; end: number } | undefined;
  readonly top: number;
  readonly bottom: number;
}

const clip = (value: number) => Math.min(1, Math.max(0, value));

// The readout at screen position (`x`, `y`), both 0 to 1 from the top left.
export function hoverReadout(
  context: HoverContext,
  x: number,
  y: number,
): HoverReadout | undefined {
  const { frame, traces } = context;
  const { sampleRate } = frame; // The rate of the audio shown.
  const covered = frame.viewEnd - frame.viewStart;
  if (traces.length === 0 || covered <= 0 || sampleRate <= 0) {
    return undefined;
  }
  const index = traceAt(traces.length, context.layout, y, context.overlaid);
  const trace = traces[index];
  const lane = lanes(traces.length, context.layout)[index];
  if (trace === undefined || lane === undefined) {
    return undefined;
  }
  const position = toFrame(context.view, x);
  const spanSeconds = frame.visibleSamples / sampleRate / covered;
  const lines: string[] = [];

  if (frame.synced && frame.spanQuarterNotes > 0) {
    const quarterNotes = frame.cycleStart + position * frame.spanQuarterNotes;
    const seconds = frame.cycleSeconds + position * spanSeconds;
    lines.push(
      `${musicalPosition(quarterNotes, frame.barQuarterNotes)} · ${formatClock(seconds)}`,
    );
  } else {
    const seconds = context.freeRunning
      ? -(1 - position) * spanSeconds
      : (position - triggerPosition) * spanSeconds;
    const text = formatSeconds(Math.abs(seconds));
    lines.push(seconds < 0 ? `−${text}` : `+${text}`);
  }

  const level = levelAt(frame, trace.values, position);
  if (level !== undefined) {
    lines.push(`${formatDbfs(level)} dBFS`);
  }

  const bounds = cycleBounds(frame, trace.values, position);
  if (bounds !== undefined) {
    // From min/max columns the cycle is approximate: rounded, and named without cents.
    const coarse = frame.mode === FrameMode.minMax;
    const seconds = (bounds.end - bounds.start) * spanSeconds;
    const samples = seconds * sampleRate;
    lines.push(
      `${coarse ? "≈ " : ""}${formatSeconds(seconds)} · ${
        coarse ? `${samples.toFixed(0)} samples` : formatSamples(samples)
      }`,
    );
    const hertz = 1 / seconds;
    const note = noteName(hertz, context.a4)?.replace(/ [+−]\d+ ct$/u, "");
    const named = coarse ? note : noteName(hertz, context.a4);
    lines.push(
      `${formatFrequency(hertz)} Hz${named === undefined ? "" : ` · ${named}`}`,
    );
  }
  const read = readPosition(frame, position);
  return {
    title: trace.name,
    lines,
    x: clip(toScreen(context.view, read ?? position)),
    cycle:
      bounds === undefined
        ? undefined
        : {
            start: clip(toScreen(context.view, bounds.start)),
            end: clip(toScreen(context.view, bounds.end)),
          },
    top: (1 - (lane.center + lane.halfHeight)) / 2,
    bottom: (1 - (lane.center - lane.halfHeight)) / 2,
  };
}
