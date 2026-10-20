// The editor's speed limits (scripts/performance-limits.json), judged against perf_bench's
// results (its --json, Bencher Metric Format) by scripts/performance.sh.
//   node scripts/performance-limits.ts scenarios            the scenarios to play, a line each:
//                                                           name, warm-up and duration (s)
//   node scripts/performance-limits.ts judge MACHINE FILE... a line per limit; exits 1 if one
//                                                           was missed
// Limits belong to a machine and a display scale: what a frame costs depends on both (the page
// draws twice the columns at twice the pixel density), so a machine or scale with no limits is
// measured and reported, not judged.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

// A bound on one measure: at least `min`, or at most `max`.
export interface Bound {
  min?: number;
  max?: number;
}

export interface LimitsFile {
  scenarios: Record<string, { warmup: number; duration: number }>;
  // Per machine, per display scale ("1", "2"), per scenario, per measure.
  machines: Record<
    string,
    {
      description: string;
      // How the limits were set: from what runs, with what margins, and why those.
      why?: string;
      limits: Record<string, Record<string, Record<string, Bound>>>;
    }
  >;
}

// Bencher Metric Format: per scenario, per measure, a value.
export type Metrics = Record<string, Record<string, { value: number }>>;

export interface Judgement {
  lines: string[];
  failed: boolean;
}

const format = (value: number): string =>
  value.toLocaleString("en-US", { maximumFractionDigits: 2 });

export function judge(
  limits: LimitsFile,
  machine: string,
  results: Metrics,
): Judgement {
  const lines: string[] = [];
  let failed = false;
  const known = limits.machines[machine];
  for (const [scenario, measures] of Object.entries(results)) {
    const scale = measures["display-scale"]?.value;
    // A machine with limits whose run could not read its scale cannot be judged, and must not
    // pass as if it had none.
    if (scale === undefined && known !== undefined) {
      lines.push(`limit: ${scenario}: display scale not measured: FAILED`);
      failed = true;
      continue;
    }
    const bounds =
      scale === undefined
        ? undefined
        : known?.limits[String(Math.round(scale))]?.[scenario];
    if (bounds === undefined) {
      lines.push(
        `limit: ${scenario}: none for ${machine} at scale ${scale === undefined ? "unknown" : format(scale)}; measured, not judged`,
      );
      continue;
    }
    for (const [measure, bound] of Object.entries(bounds)) {
      const value = measures[measure]?.value;
      if (value === undefined) {
        lines.push(`limit: ${scenario} ${measure}: not measured: FAILED`);
        failed = true;
        continue;
      }
      for (const [kind, limit] of [
        ["min", bound.min],
        ["max", bound.max],
      ] as const) {
        if (limit === undefined) {
          continue;
        }
        const ok = kind === "min" ? value >= limit : value <= limit;
        failed ||= !ok;
        lines.push(
          `limit: ${scenario} ${measure} ${format(value)} ${kind === "min" ? ">=" : "<="} ${format(limit)}: ${ok ? "ok" : "FAILED"}`,
        );
      }
    }
  }
  return { lines, failed };
}

export function readLimits(file: string): LimitsFile {
  return JSON.parse(readFileSync(file, "utf8")) as LimitsFile;
}

function main(argv: readonly string[]): number {
  const limits = readLimits(
    fileURLToPath(new URL("./performance-limits.json", import.meta.url)),
  );
  const [command, machine, ...files] = argv;
  if (command === "scenarios") {
    for (const [name, { warmup, duration }] of Object.entries(
      limits.scenarios,
    )) {
      console.log(`${name} ${String(warmup)} ${String(duration)}`);
    }
    return 0;
  }
  if (command === "judge" && machine !== undefined) {
    const results: Metrics = {};
    for (const file of files) {
      Object.assign(results, JSON.parse(readFileSync(file, "utf8")) as Metrics);
    }
    const { lines, failed } = judge(limits, machine, results);
    console.log(lines.join("\n"));
    return failed ? 1 : 0;
  }
  console.error(
    "Usage: node scripts/performance-limits.ts scenarios | judge MACHINE RESULTS.json...",
  );
  return 2;
}

if (process.argv[1]?.endsWith("performance-limits.ts") === true) {
  process.exitCode = main(process.argv.slice(2));
}
