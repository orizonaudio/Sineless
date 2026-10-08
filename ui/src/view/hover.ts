// The hover readout: what lies under the pointer on the screen. It names the trace, gives the
// position (bar.beat.sixteenth and project time in Beats mode, time from the trigger point
// otherwise), the trace's peak level there, and the length of the cycle around the pointer from
// its rising zero crossings, with that cycle's frequency and nearest note.
import { FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
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
import { toFrame, type ViewWindow } from "./zoom.ts";

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
  readonly sampleRate: number;
  readonly a4: number;
  // Seconds mode: the span rolls, so times count back from now.
  readonly roll: boolean;
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

// Span positions (0 to 1) where `values` rises through zero. A min/max column that spans zero
// rises when its neighbours' midpoints do.
export function risingCrossings(frame: Frame, values: Float32Array): number[] {
  const crossings: number[] = [];
  if (frame.mode === FrameMode.minMax) {
    const middle = (column: number) =>
      ((values[column * 2] ?? 0) + (values[column * 2 + 1] ?? 0)) / 2;
    for (let column = 1; column + 1 < frame.pointCount; column++) {
      const low = values[column * 2] ?? 0;
      const high = values[column * 2 + 1] ?? 0;
      if (low < 0 && high >= 0 && middle(column - 1) < middle(column + 1)) {
        crossings.push(columnPosition(frame, column + 0.5));
      }
    }
    return crossings;
  }
  for (let index = 0; index + 1 < frame.pointCount; index++) {
    const here = values[index] ?? 0;
    const next = values[index + 1] ?? 0;
    if (here < 0 && next >= 0) {
      crossings.push(pointPosition(frame, index + here / (here - next)));
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

// The length of the cycle around `position`, as a fraction of the span: from the last rising
// crossing at or before it to the next one after it. Undefined when either is missing, or when
// min/max columns are too coarse to tell.
export function cycleAt(
  frame: Frame,
  values: Float32Array,
  position: number,
): number | undefined {
  const crossings = risingCrossings(frame, values);
  const next = crossings.findIndex((crossing) => crossing > position);
  const after = crossings[next];
  const before = next > 0 ? crossings[next - 1] : undefined;
  if (after === undefined || before === undefined) {
    return undefined;
  }
  const length = after - before;
  if (frame.mode === FrameMode.minMax) {
    const columnWidth = (frame.viewEnd - frame.viewStart) / frame.pointCount;
    if (length < minimumColumnsPerCycle * columnWidth) {
      return undefined;
    }
  }
  return length;
}

// The readout at screen position (`x`, `y`), both 0 to 1 from the top left: a title and lines.
export function hoverReadout(
  context: HoverContext,
  x: number,
  y: number,
): { title: string; lines: string[] } | undefined {
  const { frame, traces, sampleRate } = context;
  const covered = frame.viewEnd - frame.viewStart;
  if (traces.length === 0 || covered <= 0 || sampleRate <= 0) {
    return undefined;
  }
  const trace =
    traces[traceAt(traces.length, context.layout, y, context.overlaid)];
  if (trace === undefined) {
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
    const seconds = context.roll
      ? -(1 - position) * spanSeconds
      : (position - triggerPosition) * spanSeconds;
    const text = formatSeconds(Math.abs(seconds));
    lines.push(seconds < 0 ? `−${text}` : `+${text}`);
  }

  const level = levelAt(frame, trace.values, position);
  if (level !== undefined) {
    lines.push(`${formatDbfs(level)} dBFS`);
  }

  const cycle = cycleAt(frame, trace.values, position);
  if (cycle !== undefined) {
    // From min/max columns the cycle is approximate: rounded, and named without cents.
    const coarse = frame.mode === FrameMode.minMax;
    const seconds = cycle * spanSeconds;
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
  return { title: trace.name, lines };
}
