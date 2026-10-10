// Text forms of values shown in the UI. Negative numbers use a true minus sign.

const minus = "−";

function trim(text: string): string {
  return text.includes(".")
    ? text.replace(/0+$/u, "").replace(/\.$/u, "")
    : text;
}

export function signed(value: number, decimals: number): string {
  const text = Math.abs(value).toFixed(decimals);
  if (Number(text) === 0) {
    return text;
  }
  return (value < 0 ? minus : "+") + text;
}

// A summing gain in dB with one decimal, floored like levels.
export function formatGain(db: number): string {
  return isBelowFloor(db) ? `${minus}∞` : signed(db, 1);
}

// Seconds as "0.5 ms", "20 ms", "250 ms", "1.5 s".
export function formatSeconds(seconds: number): string {
  if (seconds === 0) {
    return "0 ms";
  }
  if (Math.abs(seconds) < 1) {
    return `${trim((seconds * 1000).toPrecision(3))} ms`;
  }
  return `${trim(seconds.toPrecision(3))} s`;
}

// Project time as "m:ss.mmm", e.g. "1:04.512".
export function formatClock(seconds: number): string {
  const milliseconds = Math.round(Math.abs(seconds) * 1000);
  const minutes = Math.floor(milliseconds / 60000);
  const rest = (milliseconds % 60000) / 1000;
  const text = `${String(minutes)}:${rest.toFixed(3).padStart(6, "0")}`;
  return seconds < 0 && milliseconds > 0 ? `${minus}${text}` : text;
}

// A sample count: one decimal below 10000, e.g. "218.2 samples", "48000 samples".
export function formatSamples(samples: number): string {
  return `${samples < 10000 ? samples.toFixed(1) : samples.toFixed(0)} samples`;
}

// Readouts show −∞ at or below −100 dB, where only silence, dither or a cancelled sum's
// residue is left, rather than three-digit numbers.
const floorDb = -100;

// Whether a level reads −∞: at or below the floor once rounded to the shown decimal.
export function isBelowFloor(db: number): boolean {
  return !(Math.round(db * 10) / 10 > floorDb);
}

// Linear amplitude in dB (−Infinity for silence).
export function toDb(linear: number): number {
  return 20 * Math.log10(Math.abs(linear));
}

// Linear amplitude as dBFS with one decimal; −∞ at or below the floor.
export function formatDbfs(linear: number): string {
  const db = toDb(linear);
  if (isBelowFloor(db)) {
    return `${minus}∞`;
  }
  return db < 0 ? `${minus}${Math.abs(db).toFixed(1)}` : db.toFixed(1);
}

// A level in dB without the unit: "0", "+3", "−7.5".
export function formatDbValue(db: number): string {
  const rounded = Math.round(db * 10) / 10;
  return rounded === 0
    ? "0"
    : signed(rounded, Number.isInteger(rounded) ? 0 : 1);
}

export function formatDb(db: number): string {
  return `${formatDbValue(db)} dB`;
}

// Frequency readout: approximate, or a dash when unavailable.
export function formatFrequency(hertz: number | undefined): string {
  if (hertz === undefined) {
    return "—";
  }
  return `≈ ${hertz < 1000 ? hertz.toFixed(1) : hertz.toFixed(0)}`;
}

// Parses typed input such as "20 ms", "0.5s", "500 us", "-12 dB", "−0.25".
// Bare numbers use `defaultUnitScale` (e.g. 0.001 for milliseconds).
export function parseQuantity(
  text: string,
  defaultUnitScale = 1,
): number | undefined {
  const match =
    /^\s*([+\-−]?\d*\.?\d+(?:e[+-]?\d+)?)\s*(ms|us|µs|s|db)?\s*$/iu.exec(
      text.replace(",", "."),
    );
  if (match === null) {
    return undefined;
  }
  const value = Number((match[1] ?? "").replace(minus, "-"));
  if (!Number.isFinite(value)) {
    return undefined;
  }
  switch ((match[2] ?? "").toLowerCase()) {
    case "s":
      return value;
    case "ms":
      return value / 1000;
    case "us":
    case "µs":
      return value / 1_000_000;
    case "db":
      return value;
    default:
      return value * defaultUnitScale;
  }
}
