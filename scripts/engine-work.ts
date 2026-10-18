// The engine's work per frame, counted under Cachegrind (scripts/engine-work.sh): the results of
// one build, and the comparison of a pull request's head with its base that CI judges.
//   node scripts/engine-work.ts results DIR
//   node scripts/engine-work.ts compare BASE_DIR HEAD_DIR [AGAIN_DIR] [--accepted]
// `results` reads what engine-work.sh left in DIR and writes DIR/results.json (Bencher Metric
// Format: scenario, then measure, then value) and DIR/scenarios.json. `compare` prints Markdown
// for the job summary and exits 1 when a gated scenario's instructions per frame rose by more
// than failAbove (unless --accepted), or when the head counted differently the second time.
import { existsSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

// A gated scenario whose instructions per frame rise by more than this fails the check. Counts
// are the same on every run of one build, so the threshold only has to clear the change itself:
// 1% is a real cost, and low enough that small rises cannot add up unseen for long.
export const failAbove = 0.01;
// Changes beyond this, either way, are named as a rise or a gain, so that gains show as well: a
// third of failAbove, so that rises adding up towards it are named before one fails. Every change,
// however small, is real (counts are exact) and shows in the Change column.
export const noteAbove = 0.003;

// One scenario's run, as analysis_bench writes it (engine/bench/AnalysisBench.cpp): totals over
// the measured frames.
export interface BenchRun {
  scenario: string;
  gated: boolean;
  frames: number;
  shown: number;
  columns: number;
  samplesRead: number;
  samplesReduced: number;
  samplesSummed: number;
  samplesSearched: number;
  matchSearches: number;
}

// Bencher Metric Format: per scenario, per measure, a value.
export type Metrics = Record<string, Record<string, { value: number }>>;

export interface Results {
  metrics: Metrics;
  scenarios: Record<
    string,
    { gated: boolean; frames: number; shown: number; columns: number }
  >;
  valgrind: string;
}

export const instructions = "instructions-per-frame";
export const lastLevelMisses = "last-level-misses-per-frame";
const counted: readonly [keyof BenchRun, string][] = [
  ["samplesRead", "samples-read-per-frame"],
  ["samplesReduced", "samples-reduced-per-frame"],
  ["samplesSummed", "samples-summed-per-frame"],
  ["samplesSearched", "samples-searched-per-frame"],
  ["matchSearches", "match-searches-per-frame"],
];

// Cachegrind's totals by event name: its output's "events:" line names them, and its "summary:"
// line gives them in the same order.
export function parseCachegrind(text: string): Record<string, number> {
  const line = (key: string): string[] =>
    text
      .split("\n")
      .find((row) => row.startsWith(`${key}:`))
      ?.slice(key.length + 1)
      .trim()
      .split(/\s+/) ?? [];
  const names = line("events");
  const values = line("summary").map(Number);
  if (
    names.length === 0 ||
    names.length !== values.length ||
    values.some(Number.isNaN)
  ) {
    throw new Error(
      "not a Cachegrind output file: no events and summary lines that match",
    );
  }
  return Object.fromEntries(
    names.map((name, index) => [name, values[index] ?? 0]),
  );
}

// One scenario's measures, per measured frame.
export function scenarioMetrics(
  run: BenchRun,
  events: Record<string, number>,
): Record<string, { value: number }> {
  const perFrame = (total: number) => ({ value: total / run.frames });
  const metrics: Record<string, { value: number }> = {};
  metrics[instructions] = perFrame(events["Ir"] ?? Number.NaN);
  // With the caches simulated: misses in the last level, of data read and written.
  if (events["DLmr"] !== undefined && events["DLmw"] !== undefined) {
    metrics[lastLevelMisses] = perFrame(events["DLmr"] + events["DLmw"]);
  }
  for (const [field, name] of counted) {
    metrics[name] = perFrame(Number(run[field]));
  }
  return metrics;
}

export function readResults(dir: string): Results {
  const metrics: Metrics = {};
  const scenarios: Results["scenarios"] = {};
  for (const file of readdirSync(dir)
    .filter((name) => name.endsWith(".cachegrind"))
    .sort()) {
    const name = file.slice(0, -".cachegrind".length);
    const run = JSON.parse(
      readFileSync(join(dir, `${name}.json`), "utf8"),
    ) as BenchRun;
    metrics[name] = scenarioMetrics(
      run,
      parseCachegrind(readFileSync(join(dir, file), "utf8")),
    );
    scenarios[name] = {
      gated: run.gated,
      frames: run.frames,
      shown: run.shown,
      columns: run.columns,
    };
  }
  const valgrindFile = join(dir, "valgrind.txt");
  const valgrind = existsSync(valgrindFile)
    ? readFileSync(valgrindFile, "utf8").trim()
    : "";
  return { metrics, scenarios, valgrind };
}

// Results written by `results`, or undefined where a build left none (a base from before the
// benchmark existed, or one where it did not build).
export function loadResults(dir: string): Results | undefined {
  const file = join(dir, "results.json");
  const meta = join(dir, "scenarios.json");
  if (!existsSync(file) || !existsSync(meta)) {
    return undefined;
  }
  const { scenarios, valgrind } = JSON.parse(
    readFileSync(meta, "utf8"),
  ) as Omit<Results, "metrics">;
  return {
    metrics: JSON.parse(readFileSync(file, "utf8")) as Metrics,
    scenarios,
    valgrind,
  };
}

// Whole numbers, but for small ones, such as a match search every few frames.
const count = (value: number): string =>
  Number.isFinite(value)
    ? value.toLocaleString("en-US", {
        maximumFractionDigits: Math.abs(value) < 10 ? 2 : 0,
      })
    : "—";
const percent = (change: number): string =>
  `${change >= 0 ? "+" : "−"}${(Math.abs(change) * 100).toFixed(2)}%`;

export interface Comparison {
  markdown: string;
  failed: boolean;
}

// The head against its base: a table of every scenario's instructions per frame, and what the
// check makes of it.
export function compare(
  base: Results | undefined,
  head: Results,
  again: Results | undefined,
  accepted: boolean,
): Comparison {
  const lines = ["## Engine work per frame", ""];
  let failed = false;

  // The head counted twice, in two builds on two machines, must count the same.
  const unsteady = Object.keys(head.metrics).filter((name) => {
    const second = again?.metrics[name]?.[instructions]?.value;
    return (
      again !== undefined &&
      second !== head.metrics[name]?.[instructions]?.value
    );
  });
  if (unsteady.length > 0) {
    failed = true;
    lines.push(
      `**Not reproducible:** the head counted differently on its second build in ${unsteady.join(", ")}. ` +
        "The counts are meant to be the same on every run, so this measures the machine or the tools, not the change; it needs fixing before any comparison means anything.",
      "",
    );
  }

  if (base === undefined) {
    lines.push(
      "The base has no results (the benchmark is new there, or did not build), so there is nothing to compare.",
      "",
    );
  }
  lines.push(
    "| Scenario | Base | Head | Change | |",
    "| --- | ---: | ---: | ---: | --- |",
  );
  const rises: string[] = [];
  for (const name of Object.keys(head.metrics).sort()) {
    const now = head.metrics[name]?.[instructions]?.value ?? Number.NaN;
    const before = base?.metrics[name]?.[instructions]?.value;
    const gated = head.scenarios[name]?.gated ?? false;
    let change = "";
    let verdict = gated ? "" : "reported only";
    if (before === undefined) {
      verdict = base === undefined ? verdict : "new";
    } else {
      const ratio = now / before - 1;
      change = percent(ratio);
      if (gated && ratio > failAbove) {
        rises.push(name);
        verdict = accepted ? "rise accepted" : "**fails**";
      } else if (Math.abs(ratio) > noteAbove) {
        verdict = [verdict, ratio > 0 ? "rise" : "gain"]
          .filter((part) => part !== "")
          .join(", ");
      }
    }
    lines.push(
      `| ${name} | ${before === undefined ? "—" : count(before)} | ${count(now)} | ${change} | ${verdict} |`,
    );
  }
  for (const name of Object.keys(base?.metrics ?? {}).sort()) {
    if (head.metrics[name] === undefined) {
      lines.push(
        `| ${name} | ${count(base?.metrics[name]?.[instructions]?.value ?? Number.NaN)} | — | | removed |`,
      );
    }
  }
  lines.push("");

  if (rises.length > 0 && !accepted) {
    failed = true;
    lines.push(
      `Instructions per frame rose by more than ${(failAbove * 100).toFixed(0)}% in ${rises.join(", ")}. ` +
        "If the cost is intended, a maintainer adds the performance-accepted label with a comment saying why, and runs this job again.",
      "",
    );
  } else if (rises.length > 0) {
    lines.push(
      `Instructions per frame rose by more than ${(failAbove * 100).toFixed(0)}% in ${rises.join(", ")}, accepted with the performance-accepted label.`,
      "",
    );
  }

  // What else the head counted, per frame, for whoever looks into a change.
  lines.push(
    "<details><summary>All measures (head)</summary>",
    "",
    measuresTable(head),
    "",
    "</details>",
  );
  return { markdown: lines.join("\n"), failed };
}

// Every measure of every scenario, per frame, and what counted them.
export function measuresTable(results: Results): string {
  const measures = [
    ...new Set(Object.values(results.metrics).flatMap((m) => Object.keys(m))),
  ];
  const lines = [
    `| Scenario | ${measures.join(" | ")} |`,
    `| --- |${" ---: |".repeat(measures.length)}`,
  ];
  for (const name of Object.keys(results.metrics).sort()) {
    const values = measures.map((measure) => {
      const value = results.metrics[name]?.[measure]?.value;
      return value === undefined ? "" : count(value);
    });
    lines.push(`| ${name} | ${values.join(" | ")} |`);
  }
  lines.push(
    "",
    `Counted with ${results.valgrind === "" ? "Valgrind" : results.valgrind}.`,
  );
  return lines.join("\n");
}

function main(argv: readonly string[]): number {
  const [command, ...rest] = argv;
  if (command === "results" && rest[0] !== undefined) {
    const dir = rest[0];
    const results = readResults(dir);
    writeFileSync(
      join(dir, "results.json"),
      `${JSON.stringify(results.metrics, null, 2)}\n`,
    );
    writeFileSync(
      join(dir, "scenarios.json"),
      `${JSON.stringify({ scenarios: results.scenarios, valgrind: results.valgrind }, null, 2)}\n`,
    );
    console.log(measuresTable(results));
    return 0;
  }
  if (command === "compare") {
    const dirs = rest.filter((arg) => !arg.startsWith("--"));
    const head = dirs[1] === undefined ? undefined : loadResults(dirs[1]);
    if (dirs[0] === undefined || head === undefined) {
      console.error(
        "No results for the head: its engine-work run did not finish.",
      );
      return 1;
    }
    const again = dirs[2] === undefined ? undefined : loadResults(dirs[2]);
    const comparison = compare(
      loadResults(dirs[0]),
      head,
      again,
      rest.includes("--accepted"),
    );
    console.log(comparison.markdown);
    if (dirs[2] !== undefined && again === undefined) {
      // Without the second count, a measurement that drifts would go unseen.
      console.log(
        "\n**No second count of the head:** its run did not finish, so reproducibility is unchecked.",
      );
      return 1;
    }
    return comparison.failed ? 1 : 0;
  }
  console.error(
    "Usage: node scripts/engine-work.ts results DIR | compare BASE_DIR HEAD_DIR [AGAIN_DIR] [--accepted]",
  );
  return 2;
}

if (process.argv[1]?.endsWith("engine-work.ts") === true) {
  process.exitCode = main(process.argv.slice(2));
}
