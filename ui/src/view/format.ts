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

// Linear amplitude as dBFS with one decimal; silence is minus infinity.
export function formatDbfs(linear: number): string {
  if (linear <= 0) {
    return `${minus}∞`;
  }
  const db = 20 * Math.log10(linear);
  return db < 0 ? `${minus}${Math.abs(db).toFixed(1)}` : db.toFixed(1);
}

export function formatDb(db: number): string {
  return `${db === 0 ? "0" : signed(db, Number.isInteger(db) ? 0 : 1)} dB`;
}

// Frequency readout: approximate, or a dash when unavailable (FR-7).
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
