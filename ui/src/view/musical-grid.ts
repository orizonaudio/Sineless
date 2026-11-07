// Grid lines and labels of a tempo-synced sweep. Positions are fractions of the
// screen from the left; musical positions are in quarter notes from the project start.
import type { AxisLabel } from "./axis-labels.ts";
import { formatClock } from "./format.ts";
import {
  gridFineBeatLineGap,
  musicalTolerance,
} from "../../../protocol/src/limits.ts";
import type { BeatDivisionName, GridDensityName } from "./grid-settings.ts";
import { fullView, type ViewWindow } from "./zoom.ts";

export interface GridLine {
  readonly position: number;
  readonly strong: boolean;
}

// Indexed by BeatSpan value.
export const beatSpanLabels = [
  "1/8 bar",
  "1/6 bar",
  "1/4 bar",
  "1/3 bar",
  "1/2 bar",
  "1 bar",
  "2 bars",
  "3 bars",
  "4 bars",
  "6 bars",
  "8 bars",
  "16 bars",
] as const;

// The beat span a typed text names: a label, or its number alone ("16" for 16 bars, "1/4" for
// 1/4 bar), with or without the unit and spaces.
export function parseBeatSpan(text: string): number | undefined {
  const amount = (label: string) =>
    label
      .toLowerCase()
      .replace(/\s*bars?$/u, "")
      .replace(/\s+/gu, "");
  const typed = amount(text.trim());
  const index = beatSpanLabels.findIndex((label) => amount(label) === typed);
  return index < 0 ? undefined : index;
}

const maximumLabels = 5;
const epsilon = musicalTolerance;

function greatestCommonDivisor(a: number, b: number): number {
  return b === 0 ? a : greatestCommonDivisor(b, a % b);
}

// Bar.beat.sixteenth, as DAWs count; whole beats drop the sixteenth.
export function musicalPosition(
  quarterNotes: number,
  barQuarterNotes: number,
  // Keep the sixteenth on whole beats too ("25.2.1"), as the hover readout does, so its position
  // keeps one shape as the pointer moves.
  sixteenthAlways = false,
): string {
  const bar = Math.floor(quarterNotes / barQuarterNotes + epsilon);
  const inBar = Math.max(0, quarterNotes - bar * barQuarterNotes);
  const beat = Math.floor(inBar + epsilon);
  const sixteenth = Math.floor((inBar - beat) * 4 + epsilon);
  const base = `${String(bar + 1)}.${String(beat + 1)}`;
  return sixteenth === 0 && !sixteenthAlways
    ? base
    : `${base}.${String(sixteenth + 1)}`;
}

// An offset within a beat as a fraction of a beat, reduced from `denominator`ths: "+1/64", or in
// Triplets "+1/3".
function beatFraction(quarterNotes: number, denominator: number): string {
  const parts = Math.round(quarterNotes * denominator);
  const divisor = greatestCommonDivisor(parts, denominator);
  return `+${String(parts / divisor)}/${String(denominator / divisor)}`;
}

// The finest step Beats draws, in quarter notes: the Regular ladder ends on it, the Triplets ladder
// on the first of its own below it.
const finestStep = 1 / 1024;

// Beats' grid levels, in quarter notes, longest first, a level as long as another merged with it:
// 16 bars, 4 bars, a bar, a beat (a quarter note, FR-9.1), then below the beat sixteenths and their
// halves (Regular) or thirds of the beat and their halves (Triplets).
export function beatLadder(
  barQuarterNotes: number,
  division: BeatDivisionName,
): number[] {
  const levels = [
    16 * barQuarterNotes,
    4 * barQuarterNotes,
    barQuarterNotes,
    1,
  ];
  for (let level = division === "triplets" ? 1 / 3 : 1 / 4; ; level /= 2) {
    levels.push(level);
    if (level <= finestStep) {
      break;
    }
  }
  levels.sort((a, b) => b - a);
  return levels.filter(
    (level, index) =>
      index === 0 || (levels[index - 1] ?? 0) - level > epsilon * level,
  );
}

// Which Beats grid to draw: the Time step and Beat division, and the screen's width in CSS pixels,
// which Fine's finer step must keep gridFineBeatLineGap apart on.
export interface BeatGridOptions {
  readonly time: GridDensityName;
  readonly division: BeatDivisionName;
  readonly widthCss: number;
}

const mediumBeatGrid: BeatGridOptions = {
  time: "medium",
  division: "regular",
  widthCss: Number.POSITIVE_INFINITY,
};

// The step and the strong lines' step of a visible span `visibleSpan` quarter notes long, in
// quarter notes:
// - Medium: bars (8 bars or more, strong every 4 bars), beats (a bar or more, strong bars), else
//   the first level below the beat (strong beats), refined while fewer than 4 steps show (Regular
//   halves a beat, as before; Triplets goes to its thirds); in Triplets, a step below the beat is
//   strong on the next coarser Triplets level, not on a sixteenth.
// - Coarse: one ladder level coarser than Medium; Fine one finer, where its lines stay
//   gridFineBeatLineGap CSS pixels apart (else Medium). Both strong one level coarser still.
export function beatSteps(
  visibleSpan: number,
  barQuarterNotes: number,
  options: BeatGridOptions,
): { step: number; strongEvery: number } {
  const ladder = beatLadder(barQuarterNotes, options.division);
  const triplets = options.division === "triplets";
  const coarser = (length: number) =>
    [...ladder].reverse().find((level) => level - length > epsilon * length) ??
    4 * length;
  const finer = (length: number) =>
    ladder.find((level) => length - level > epsilon * length);

  const longView = visibleSpan >= 8 * barQuarterNotes;
  let step = longView
    ? barQuarterNotes
    : visibleSpan >= barQuarterNotes
      ? 1
      : triplets
        ? 1 / 3
        : 0.25;
  while (visibleSpan < 4 * step && step > finestStep) {
    step = triplets && Math.abs(step - 1) < epsilon ? 1 / 3 : step / 2;
  }
  const strongEvery =
    triplets && step < 1 - epsilon
      ? coarser(step)
      : longView
        ? 4 * barQuarterNotes
        : visibleSpan >= barQuarterNotes
          ? barQuarterNotes
          : step < 0.25
            ? 0.25
            : 1;
  if (options.time === "coarse") {
    const coarse = coarser(step);
    return { step: coarse, strongEvery: coarser(coarse) };
  }
  if (options.time === "fine") {
    const fine = finer(step);
    if (
      fine !== undefined &&
      (fine / visibleSpan) * options.widthCss >= gridFineBeatLineGap
    ) {
      return { step: fine, strongEvery: coarser(fine) };
    }
  }
  return { step, strongEvery };
}

// Lines and labels over the visible part of a sweep. `cycleStart` and `span` (quarter notes)
// describe the whole frame; `view` the part on screen. Lines sit on absolute musical positions,
// so they refine as the view narrows (beatSteps), at the Time step and Beat division `grid` names.
// With `timing` (the left edge's project time and the seconds per quarter note), labels carry
// their project time too.
export function musicalGrid(
  cycleStart: number,
  span: number,
  barQuarterNotes: number,
  view: ViewWindow = fullView,
  timing?: {
    readonly cycleSeconds: number;
    readonly secondsPerQuarter: number;
  },
  grid: BeatGridOptions = mediumBeatGrid,
): { lines: GridLine[]; labels: AxisLabel[] } {
  if (span <= 0 || barQuarterNotes <= 0) {
    return { lines: [], labels: [] };
  }
  const visibleStart = cycleStart + view.start * span;
  const visibleSpan = (view.end - view.start) * span;
  const { step, strongEvery } = beatSteps(visibleSpan, barQuarterNotes, grid);
  const triplets = grid.division === "triplets";
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

  // Labels step up from the line step by doubling, except that thirds of a beat go to the beat
  // (two thirds would put labels off the beats).
  let labelStep = step;
  while (visibleSpan / labelStep > maximumLabels) {
    labelStep =
      triplets && Math.abs(labelStep - 1 / 3) < epsilon ? 1 : labelStep * 2;
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
      : beatFraction(
          quarterNotes - Math.floor(quarterNotes + epsilon),
          triplets ? 192 : 64,
        );
    labels.push({
      text,
      position: position(quarterNotes),
      ...(timing === undefined
        ? {}
        : {
            detail: formatClock(
              timing.cycleSeconds +
                (quarterNotes - cycleStart) * timing.secondsPerQuarter,
            ),
          }),
    });
  }
  return { lines, labels };
}
