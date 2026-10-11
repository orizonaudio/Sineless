// Text forms of values shown in the UI. Negative numbers use a true minus sign; a value that is
// missing, or not a number (from a broken signal upstream), shows as a dash. Every formatter
// rounds first and then chooses its unit and decimals, so a value just under a boundary ("999.96")
// shows as the value it rounds to ("1000"), and none uses exponent notation.
import { gainFloorDb, levelFloorDbfs } from "../../../protocol/src/limits.ts";

const minus = "−";
const none = "—";

function trim(text: string): string {
  return text.includes(".")
    ? text.replace(/0+$/u, "").replace(/\.$/u, "")
    : text;
}

// `value` to `digits` significant digits, as plain decimals without trailing zeros.
function significant(value: number, digits: number): string {
  if (value === 0) {
    return "0";
  }
  const decimals = Math.max(
    0,
    digits - 1 - Math.floor(Math.log10(Math.abs(value))),
  );
  return trim(value.toFixed(decimals));
}

// `value` rounded to `decimals`, as a number.
function rounded(value: number, decimals: number): number {
  return Number(value.toFixed(decimals));
}

export function signed(value: number, decimals: number): string {
  const text = Math.abs(value).toFixed(decimals);
  if (Number(text) === 0) {
    return text;
  }
  return (value < 0 ? minus : "+") + text;
}

// A summing gain with one decimal: "+3.0 dB", "−∞ dB" at or below the gain floor.
export function formatGain(db: number): string {
  if (Number.isNaN(db) || db === Infinity) {
    return none;
  }
  return `${isBelowFloor(db, gainFloorDb) ? `${minus}∞` : signed(db, 1)} dB`;
}

// Seconds to three significant digits: "0.5 ms", "20 ms", "250 ms", "1.5 s".
export function formatSeconds(seconds: number): string {
  if (!Number.isFinite(seconds)) {
    return none;
  }
  const milliseconds = Number(significant(seconds * 1000, 3));
  if (Math.abs(milliseconds) < 1000) {
    return `${significant(milliseconds, 3).replace("-", minus)} ms`;
  }
  return `${significant(seconds, 3).replace("-", minus)} s`;
}

// A track's offset in milliseconds, signed: one decimal from 1 ms, and below it two significant
// digits, so that a shift of a single sample shows at any rate ("+0.021 ms" at 48 kHz, "+0.0026 ms"
// at 384 kHz): "0.0 ms", "+0.52 ms", "−1.5 ms".
export function formatOffset(milliseconds: number): string {
  if (!Number.isFinite(milliseconds)) {
    return none;
  }
  if (milliseconds === 0) {
    return "0.0 ms";
  }
  if (Number(significant(Math.abs(milliseconds), 2)) >= 1) {
    return `${signed(milliseconds, 1)} ms`;
  }
  const text = significant(Math.abs(milliseconds), 2);
  return `${milliseconds < 0 ? minus : "+"}${text} ms`;
}

// Project time as "m:ss.mmm", e.g. "1:04.512".
export function formatClock(seconds: number): string {
  if (!Number.isFinite(seconds)) {
    return none;
  }
  const milliseconds = Math.round(Math.abs(seconds) * 1000);
  const minutes = Math.floor(milliseconds / 60000);
  const rest = (milliseconds % 60000) / 1000;
  const text = `${String(minutes)}:${rest.toFixed(3).padStart(6, "0")}`;
  return seconds < 0 && milliseconds > 0 ? `${minus}${text}` : text;
}

// A sample count: one decimal below 10000, e.g. "218.2 samples", "48000 samples".
export function formatSamples(samples: number): string {
  if (!Number.isFinite(samples)) {
    return none;
  }
  const decimals = rounded(samples, 1) < 10000 ? 1 : 0;
  return `${samples.toFixed(decimals)} samples`;
}

// Whether `db` reads −∞: at or below `floor` once rounded to the shown decimal. Levels use the
// level floor; summing gains, which are ratios, their own.
export function isBelowFloor(db: number, floor = levelFloorDbfs): boolean {
  return Math.round(db * 10) / 10 <= floor;
}

// Linear amplitude in dB (−Infinity for silence).
export function toDb(linear: number): number {
  return 20 * Math.log10(Math.abs(linear));
}

// Linear amplitude as dBFS with one decimal; −∞ at or below the level floor.
export function formatDbfs(linear: number): string {
  if (!Number.isFinite(linear)) {
    return none;
  }
  const db = toDb(linear);
  if (isBelowFloor(db)) {
    return `${minus}∞`;
  }
  const shown = rounded(db, 1); // −0.04 dB reads 0.0, not −0.0.
  return shown < 0 ? `${minus}${Math.abs(shown).toFixed(1)}` : shown.toFixed(1);
}

// A level in dB without the unit: "0", "+3", "−7.5".
export function formatDbValue(db: number): string {
  if (!Number.isFinite(db)) {
    return none;
  }
  const rounded = Math.round(db * 10) / 10;
  return rounded === 0
    ? "0"
    : signed(rounded, Number.isInteger(rounded) ? 0 : 1);
}

export function formatDb(db: number): string {
  return Number.isFinite(db) ? `${formatDbValue(db)} dB` : none;
}

// Frequency readout: approximate, or a dash when unavailable.
export function formatFrequency(hertz: number | undefined): string {
  if (hertz === undefined || !Number.isFinite(hertz)) {
    return none;
  }
  return `≈ ${hertz.toFixed(rounded(hertz, 1) < 1000 ? 1 : 0)}`;
}

// A reference pitch, to a tenth of a hertz without trailing zeros: "440", "441.5".
export function formatPitch(hertz: number): string {
  if (!Number.isFinite(hertz)) {
    return none;
  }
  return trim(hertz.toFixed(1));
}

// What an editable field holds, and so which units typed into it are accepted.
export type Quantity = "time" | "level" | "pitch" | "number";

const unitScales: Record<Quantity, Record<string, number>> = {
  time: { s: 1, ms: 0.001, us: 0.000001, µs: 0.000001 },
  level: { db: 1 },
  pitch: { hz: 1, khz: 1000 },
  number: {},
};

// Parses typed input such as "20 ms", "0.5s", "500 us", "-12 dB", "440 Hz", "−0.25", accepting
// only the units of `quantity`. Bare numbers are scaled by `bareScale` (e.g. 0.001 when a time
// field's bare numbers are milliseconds).
export function parseQuantity(
  text: string,
  quantity: Quantity,
  bareScale = 1,
): number | undefined {
  const match = /^\s*([+\-−]?\d*\.?\d+(?:e[+-]?\d+)?)\s*([a-zµ]*)\s*$/iu.exec(
    text.replace(",", "."),
  );
  if (match === null) {
    return undefined;
  }
  const value = Number((match[1] ?? "").replace(minus, "-"));
  const unit = (match[2] ?? "").toLowerCase();
  const scale = unit === "" ? bareScale : unitScales[quantity][unit];
  if (!Number.isFinite(value) || scale === undefined) {
    return undefined;
  }
  return value * scale;
}
