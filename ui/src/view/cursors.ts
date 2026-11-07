// Cursors. Time cursors are two vertical lines, A and B, at positions in the frame (fractions of
// the span), so that they move with the waveform when the view zooms or pans. Level cursors are
// two horizontal lines, 1 and 2, at sample values, so that they follow the vertical scale.
// Temporary display state only; they are never saved.
import {
  formatMusicalLength,
  formatDbDifference,
  formatDbfs,
  formatFrequency,
  formatSamples,
  formatSeconds,
  isBelowFloor,
  toDb,
} from "./format.ts";
import { toFrame, type ViewWindow } from "./zoom.ts";

export interface CursorPair {
  readonly a: number;
  readonly b: number;
}

export interface CursorMeasurement {
  readonly seconds: number;
  // 1 / seconds; undefined when the cursors coincide.
  readonly hertz: number | undefined;
  // The distance in beats (quarter notes), in a tempo-synced sweep only.
  readonly quarterNotes: number | undefined;
  // The bar's length in beats, in a tempo-synced sweep only.
  readonly barQuarterNotes?: number;
}

// New cursors sit a third and two thirds across the screen.
export function initialCursors(view: ViewWindow): CursorPair {
  return { a: toFrame(view, 1 / 3), b: toFrame(view, 2 / 3) };
}

// The distance between the cursors for a span of `spanSeconds`, and of `spanQuarterNotes` in a
// tempo-synced sweep.
export function measure(
  cursors: CursorPair,
  spanSeconds: number,
  spanQuarterNotes?: number,
  barQuarterNotes?: number,
): CursorMeasurement {
  const fraction = Math.abs(cursors.b - cursors.a);
  const seconds = fraction * spanSeconds;
  return {
    seconds,
    hertz: seconds > 0 ? 1 / seconds : undefined,
    quarterNotes:
      spanQuarterNotes === undefined ? undefined : fraction * spanQuarterNotes,
    ...(barQuarterNotes === undefined ? {} : { barQuarterNotes }),
  };
}

const noteNames = [
  "C",
  "C#",
  "D",
  "D#",
  "E",
  "F",
  "F#",
  "G",
  "G#",
  "A",
  "A#",
  "B",
];

// The nearest equal-tempered note, tuned to `a4` Hz, and the offset from it in cents, e.g.
// "A3 +2 ct". Undefined outside the audible range.
export function noteName(hertz: number, a4 = 440): string | undefined {
  if (!(hertz >= 20 && hertz <= 20000)) {
    return undefined;
  }
  const semitones = 69 + 12 * Math.log2(hertz / a4);
  const note = Math.round(semitones);
  const cents = Math.round((semitones - note) * 100);
  const name = `${noteNames[note % 12] ?? ""}${String(Math.floor(note / 12) - 1)}`;
  if (cents === 0) {
    return name;
  }
  return `${name} ${cents > 0 ? "+" : "−"}${String(Math.abs(cents))} ct`;
}

// One line of a cursor readout: what it is, then its value, as a popover's rows.
export interface ReadoutRow {
  readonly label: string;
  readonly value: string;
}

// The readout rows: where A and B are (`times`, when known), the distance between them, its
// frequency and nearest note (tuned to `a4`), the distance in beats, and in samples at
// `sampleRate`.
export function cursorReadout(
  measurement: CursorMeasurement,
  sampleRate?: number,
  a4 = 440,
  times?: { readonly a: string; readonly b: string },
): ReadoutRow[] {
  const rows: ReadoutRow[] = [];
  if (times !== undefined) {
    rows.push({ label: "A", value: times.a }, { label: "B", value: times.b });
  }
  rows.push({ label: "Δt", value: formatSeconds(measurement.seconds) });
  // Every row stays, a dash when it has no value, so the readout keeps its shape.
  rows.push({
    label: "f",
    value:
      measurement.hertz === undefined
        ? "—"
        : `${formatFrequency(measurement.hertz)} Hz`,
  });
  rows.push({
    label: "Note",
    value:
      (measurement.hertz === undefined
        ? undefined
        : noteName(measurement.hertz, a4)) ?? "—",
  });
  if (measurement.quarterNotes !== undefined) {
    rows.push({
      label: "Δb",
      value: formatMusicalLength(
        measurement.quarterNotes,
        measurement.barQuarterNotes ?? Number.NaN,
      ),
    });
  }
  if (sampleRate !== undefined) {
    rows.push({
      label: "Δs",
      value: formatSamples(measurement.seconds * sampleRate),
    });
  }
  return rows;
}

export interface LevelPair {
  readonly first: number;
  readonly second: number;
}

// New level cursors sit at half and a quarter of full scale: 6 and 12 dB below the top.
export function initialLevels(fullScale: number): LevelPair {
  return { first: fullScale / 2, second: fullScale / 4 };
}

// The readout rows: each level in dBFS, and the difference between them in dB.
export function levelReadout(levels: LevelPair): ReadoutRow[] {
  const first = toDb(levels.first);
  const second = toDb(levels.second);
  const difference =
    isBelowFloor(first) || isBelowFloor(second)
      ? Number.NaN
      : Math.abs(first - second);
  return [
    { label: "1", value: `${formatDbfs(Math.abs(levels.first))} dBFS` },
    { label: "2", value: `${formatDbfs(Math.abs(levels.second))} dBFS` },
    { label: "\u0394", value: formatDbDifference(difference) },
  ];
}
