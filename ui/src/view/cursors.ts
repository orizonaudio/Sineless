// Cursors. Time cursors are two vertical lines, A and B, at positions in the frame (fractions of
// the span), so that they move with the waveform when the view zooms or pans. Level cursors are
// two horizontal lines, 1 and 2, at sample values, so that they follow the vertical scale.
// Temporary display state only; they are never saved.
import {
  formatDbfs,
  formatFrequency,
  formatSamples,
  formatSeconds,
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

// The readout lines: the distance, in samples at `sampleRate` too, its frequency with the nearest
// note (tuned to `a4`), and the distance in beats.
export function cursorReadout(
  measurement: CursorMeasurement,
  sampleRate?: number,
  a4 = 440,
): string[] {
  const lines = [`Δt ${formatSeconds(measurement.seconds)}`];
  if (sampleRate !== undefined) {
    lines.push(formatSamples(measurement.seconds * sampleRate));
  }
  if (measurement.hertz !== undefined) {
    const note = noteName(measurement.hertz, a4);
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

export interface LevelPair {
  readonly first: number;
  readonly second: number;
}

// New level cursors sit at half and a quarter of full scale: 6 and 12 dB below the top.
export function initialLevels(fullScale: number): LevelPair {
  return { first: fullScale / 2, second: fullScale / 4 };
}

// The readout lines: each level in dBFS, and the difference between them in dB.
export function levelReadout(levels: LevelPair): string[] {
  const first = 20 * Math.log10(Math.abs(levels.first));
  const second = 20 * Math.log10(Math.abs(levels.second));
  const difference = Math.abs(first - second);
  return [
    `1 ${formatDbfs(Math.abs(levels.first))} dBFS`,
    `2 ${formatDbfs(Math.abs(levels.second))} dBFS`,
    `\u0394 ${Number.isFinite(difference) ? difference.toFixed(1) : "\u2014"} dB`,
  ];
}
