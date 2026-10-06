// Grid lines and labels of a tempo-synced sweep (DESIGN 8.5). Positions are fractions of the
// screen from the left; musical positions are in quarter notes from the project start.
import type { AxisLabel } from "./axis-labels.ts";

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

export function musicalGrid(
  cycleStart: number,
  span: number,
  barQuarterNotes: number,
): { lines: GridLine[]; labels: AxisLabel[] } {
  if (span <= 0 || barQuarterNotes <= 0) {
    return { lines: [], labels: [] };
  }
  // Beats with stronger bars; sixteenths with stronger beats; quarters of a sub-beat span.
  const step = span >= barQuarterNotes ? 1 : span >= 1 ? 0.25 : span / 4;
  const strongEvery =
    span >= barQuarterNotes ? barQuarterNotes : span >= 1 ? 1 : Infinity;
  const lines: GridLine[] = [];
  const count = Math.round(span / step);
  for (let index = 1; index < count; index++) {
    const offset = index * step;
    const absolute = cycleStart + offset;
    const strong =
      Number.isFinite(strongEvery) &&
      Math.abs(absolute / strongEvery - Math.round(absolute / strongEvery)) <
        epsilon;
    lines.push({ position: offset / span, strong });
  }

  let labelStep = step;
  while (span / labelStep > maximumLabels) {
    labelStep *= 2;
  }
  const labels: AxisLabel[] = [];
  for (let offset = 0; offset < span - epsilon; offset += labelStep) {
    const text =
      span < 1 && offset > 0
        ? beatFraction(offset)
        : musicalPosition(cycleStart + offset, barQuarterNotes);
    labels.push({ text, position: offset / span });
  }
  return { lines, labels };
}
