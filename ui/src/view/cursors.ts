// Time cursors: two vertical lines, A and B, at positions in the frame (fractions of the span),
// so that they move with the waveform when the view zooms or pans. Temporary display state only;
// they are never saved.
import { formatFrequency, formatSeconds } from "./format.ts";
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
): CursorMeasurement {
  const fraction = Math.abs(cursors.b - cursors.a);
  const seconds = fraction * spanSeconds;
  return {
    seconds,
    hertz: seconds > 0 ? 1 / seconds : undefined,
    quarterNotes:
      spanQuarterNotes === undefined ? undefined : fraction * spanQuarterNotes,
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

// The nearest equal-tempered note (A4 = 440 Hz) and the offset from it in cents, e.g. "A3 +2 ct".
// Undefined outside the audible range.
export function noteName(hertz: number): string | undefined {
  if (!(hertz >= 20 && hertz <= 20000)) {
    return undefined;
  }
  const semitones = 69 + 12 * Math.log2(hertz / 440);
  const note = Math.round(semitones);
  const cents = Math.round((semitones - note) * 100);
  const name = `${noteNames[note % 12] ?? ""}${String(Math.floor(note / 12) - 1)}`;
  if (cents === 0) {
    return name;
  }
  return `${name} ${cents > 0 ? "+" : "−"}${String(Math.abs(cents))} ct`;
}

// The readout lines: the distance, its frequency with the nearest note, and the distance in beats.
export function cursorReadout(measurement: CursorMeasurement): string[] {
  const lines = [`Δt ${formatSeconds(measurement.seconds)}`];
  if (measurement.hertz !== undefined) {
    const note = noteName(measurement.hertz);
    lines.push(
      `${formatFrequency(measurement.hertz)} Hz${note === undefined ? "" : ` · ${note}`}`,
    );
  }
  if (measurement.quarterNotes !== undefined) {
    const beats = Number(measurement.quarterNotes.toPrecision(3));
    lines.push(`${String(beats)} ${beats === 1 ? "beat" : "beats"}`);
  }
  return lines;
}
