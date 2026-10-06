// Grid lines and labels of a tempo-synced sweep (DESIGN 8.5). Positions are fractions of the
// screen from the left; musical positions are in quarter notes from the project start.
import type { AxisLabel } from "./axis-labels.ts";
import { fullView, type ViewWindow } from "./zoom.ts";

export interface GridLine {
  readonly position: number;
  readonly strong: boolean;
}

export const beatSpanLabels = [
  "1/16 beat",
  "1/8 beat",
  "1/4 beat",
  "1/2 beat",
  "1 beat",
  "2 beats",
  "1 bar",
  "2 bars",
  "4 bars",
] as const;

const maximumLabels = 5;
const epsilon = 1e-6;

function greatestCommonDivisor(a: number, b: number): number {
  return b === 0 ? a : greatestCommonDivisor(b, a % b);
}

// Bar.beat.sixteenth, as DAWs count; whole beats drop the sixteenth.
export function musicalPosition(
  quarterNotes: number,
  barQuarterNotes: number,
): string {
  const bar = Math.floor(quarterNotes / barQuarterNotes + epsilon);
  const inBar = Math.max(0, quarterNotes - bar * barQuarterNotes);
  const beat = Math.floor(inBar + epsilon);
  const sixteenth = Math.floor((inBar - beat) * 4 + epsilon);
  const base = `${String(bar + 1)}.${String(beat + 1)}`;
  return sixteenth === 0 ? base : `${base}.${String(sixteenth + 1)}`;
}

// An offset within a beat as a fraction of a beat, e.g. "+1/64".
function beatFraction(quarterNotes: number): string {
  const sixtyFourths = Math.round(quarterNotes * 64);
  const divisor = greatestCommonDivisor(sixtyFourths, 64);
  return `+${String(sixtyFourths / divisor)}/${String(64 / divisor)}`;
}

// Lines and labels over the visible part of a sweep. `cycleStart` and `span` (quarter notes)
// describe the whole frame; `view` the part on screen (FR-10). Lines sit on absolute musical
// positions, so they refine as the view narrows: beats with stronger bars, then sixteenths with
// stronger beats, then binary fractions of a beat.
export function musicalGrid(
  cycleStart: number,
  span: number,
  barQuarterNotes: number,
  view: ViewWindow = fullView,
): { lines: GridLine[]; labels: AxisLabel[] } {
  if (span <= 0 || barQuarterNotes <= 0) {
    return { lines: [], labels: [] };
  }
  const visibleStart = cycleStart + view.start * span;
  const visibleSpan = (view.end - view.start) * span;
  let step = visibleSpan >= barQuarterNotes ? 1 : 0.25;
  while (visibleSpan < 4 * step && step > 1 / 1024) {
    step /= 2;
  }
  const strongEvery =
    visibleSpan >= barQuarterNotes ? barQuarterNotes : step < 0.25 ? 0.25 : 1;
  const isMultiple = (value: number, of: number) =>
    Math.abs(value / of - Math.round(value / of)) < epsilon;
  const position = (quarterNotes: number) =>
    (quarterNotes - visibleStart) / visibleSpan;

  const lines: GridLine[] = [];
  for (
    let index = Math.floor(visibleStart / step) + 1;
    index * step < visibleStart + visibleSpan - epsilon;
    index++
  ) {
    const quarterNotes = index * step;
    if (quarterNotes > visibleStart + epsilon) {
      lines.push({
        position: position(quarterNotes),
        strong: isMultiple(quarterNotes, strongEvery),
      });
    }
  }

  let labelStep = step;
  while (visibleSpan / labelStep > maximumLabels) {
    labelStep *= 2;
  }
  const labels: AxisLabel[] = [];
  for (
    let index = Math.ceil(visibleStart / labelStep - epsilon);
    index * labelStep < visibleStart + visibleSpan - epsilon;
    index++
  ) {
    const quarterNotes = index * labelStep;
    const text = isMultiple(quarterNotes, 0.25)
      ? musicalPosition(quarterNotes, barQuarterNotes)
      : beatFraction(quarterNotes - Math.floor(quarterNotes + epsilon));
    labels.push({ text, position: position(quarterNotes) });
  }
  return { lines, labels };
}
