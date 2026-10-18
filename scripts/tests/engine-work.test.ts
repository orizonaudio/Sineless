import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { fileURLToPath } from "node:url";

import { describe, expect, test } from "vitest";

import {
  compare,
  failAbove,
  instructions,
  lastLevelMisses,
  noteAbove,
  parseBenchRun,
  parseCachegrind,
  readResults,
  type BenchRun,
  type Results,
} from "../engine-work.ts";

// A results set with one gated and one reported-only scenario, at these instructions per frame.
const results = (gated: number, reported = 1000): Results => ({
  metrics: {
    "compare-15-worst": { [instructions]: { value: gated } },
    "multi-15-zoomed": { [instructions]: { value: reported } },
  },
  scenarios: {
    "compare-15-worst": { gated: true, frames: 300, shown: 300, columns: 2560 },
    "multi-15-zoomed": { gated: false, frames: 300, shown: 300, columns: 2560 },
  },
  valgrind: "valgrind-3.22.0",
});

describe("parseCachegrind", () => {
  test("names each total by the event the events line lists", () => {
    const text =
      "desc: x\ncmd: bench\nevents: Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw\nfl=a\n1 2\nsummary: 900 1 2 300 4 5 200 7 8\n";
    expect(parseCachegrind(text)).toEqual({
      Ir: 900,
      I1mr: 1,
      ILmr: 2,
      Dr: 300,
      D1mr: 4,
      DLmr: 5,
      Dw: 200,
      D1mw: 7,
      DLmw: 8,
    });
  });

  test("refuses a file that is not Cachegrind's, rather than counting nothing", () => {
    expect(() => parseCachegrind("events: Ir\n")).toThrow(
      /not a Cachegrind output/,
    );
    expect(() => parseCachegrind("events: Ir Dr\nsummary: 1\n")).toThrow();
    expect(() => parseCachegrind("")).toThrow(); // A file cut short.
    expect(() => parseCachegrind("events: Ir\nsummary: x\n")).toThrow();
  });
});

// A run as analysis_bench writes it, with every field the gate reads.
const benchRun: BenchRun = {
  scenario: "compare-15-worst",
  gated: true,
  frames: 300,
  shown: 300,
  columns: 2560,
  samplesRead: 3000,
  samplesReduced: 600,
  samplesSummed: 1500,
  samplesSearched: 900,
  matchSearches: 50,
};

describe("parseBenchRun", () => {
  test("reads a run with every field the gate reads", () => {
    expect(parseBenchRun(JSON.stringify(benchRun), "compare-15-worst")).toEqual(
      benchRun,
    );
  });

  // Without any one of them the gate would judge nothing, or NaN: refused, naming the run.
  test.each(Object.keys(benchRun))(
    "refuses a run without %s, rather than letting any rise pass",
    (field) => {
      const run = Object.fromEntries(
        Object.entries(benchRun).filter(([key]) => key !== field),
      );
      expect(() =>
        parseBenchRun(JSON.stringify(run), "compare-15-worst"),
      ).toThrow(/compare-15-worst\.json/);
    },
  );

  test("refuses a gated that is not true or false, no frames, or another scenario's run", () => {
    const parse = (changes: Record<string, unknown>) => () =>
      parseBenchRun(
        JSON.stringify({ ...benchRun, ...changes }),
        "compare-15-worst",
      );
    expect(parse({ gated: "yes" })).toThrow(/gated/);
    expect(parse({ frames: 0 })).toThrow(/frames/);
    expect(parse({ samplesRead: null })).toThrow(/samplesRead/);
    expect(parse({ scenario: "single-one-bar" })).toThrow(/not the run/);
  });
});

describe("readResults", () => {
  test("turns a run's totals into measures per measured frame, cache misses where simulated", () => {
    const dir = mkdtempSync(join(tmpdir(), "engine-work-"));
    const run = benchRun;
    writeFileSync(join(dir, "compare-15-worst.json"), JSON.stringify(run));
    writeFileSync(
      join(dir, "compare-15-worst.cachegrind"),
      "events: Ir DLmr DLmw\nsummary: 30000 60 30\n",
    );
    writeFileSync(
      join(dir, "single-one-bar.json"),
      JSON.stringify({ ...run, scenario: "single-one-bar", gated: false }),
    );
    writeFileSync(
      join(dir, "single-one-bar.cachegrind"),
      "events: Ir\nsummary: 600\n",
    );
    writeFileSync(join(dir, "valgrind.txt"), "valgrind-3.22.0\n");

    const read = readResults(dir);
    expect(read.metrics["compare-15-worst"]).toEqual({
      [instructions]: { value: 100 },
      [lastLevelMisses]: { value: 0.3 },
      "samples-read-per-frame": { value: 10 },
      "samples-reduced-per-frame": { value: 2 },
      "samples-summed-per-frame": { value: 5 },
      "samples-searched-per-frame": { value: 3 },
      "match-searches-per-frame": { value: 50 / 300 },
    });
    expect(read.metrics["single-one-bar"]?.[lastLevelMisses]).toBeUndefined();
    expect(read.scenarios["single-one-bar"]?.gated).toBe(false);
    expect(read.scenarios["compare-15-worst"]?.gated).toBe(true);
    expect(read.valgrind).toBe("valgrind-3.22.0");
  });
});

describe("compare", () => {
  test("fails when a gated scenario's instructions per frame rise beyond the threshold", () => {
    const base = 1_000_000;
    const result = compare(
      results(base),
      results(base * (1 + failAbove * 1.1)),
      undefined,
      false,
    );
    expect(result.failed).toBe(true);
    expect(result.markdown).toContain("**fails**");
    expect(result.markdown).toContain("performance-accepted");
    // The table shows both counts and the change, as the maintainer judges by them.
    expect(
      compare(results(base), results(base * 2), undefined, false).markdown,
    ).toMatch(/\| compare-15-worst \| 1,000,000 \| 2,000,000 \| \+100\.00% \|/);
  });

  test("passes a rise within the threshold, and names a notable one", () => {
    const base = 1_000_000;
    const within = compare(
      results(base),
      results(base * (1 + failAbove * 0.9)),
      undefined,
      false,
    );
    expect(within.failed).toBe(false);
    expect(within.markdown).toContain("rise");
    const notable = compare(
      results(base),
      results(base * (1 + (noteAbove + failAbove) / 2)),
      undefined,
      false,
    );
    expect(notable.failed).toBe(false);
    expect(notable.markdown).toMatch(/\| rise \|/);
    const tiny = compare(
      results(base),
      results(base * (1 + noteAbove / 2)),
      undefined,
      false,
    );
    expect(tiny.markdown).not.toMatch(/\| rise/);
  });

  test("passes a rise a maintainer accepted, and says so", () => {
    const result = compare(results(1000), results(2000), undefined, true);
    expect(result.failed).toBe(false);
    expect(result.markdown).toContain("rise accepted");
  });

  test("lists a gain, and never fails on one", () => {
    const result = compare(results(1000), results(500), undefined, false);
    expect(result.failed).toBe(false);
    expect(result.markdown).toContain("gain");
  });

  test("reports a reported-only scenario's rise without failing", () => {
    const result = compare(
      results(1000, 1000),
      results(1000, 5000),
      undefined,
      false,
    );
    expect(result.failed).toBe(false);
    expect(result.markdown).toMatch(/multi-15-zoomed .*reported only, rise/);
  });

  test("fails when the head counts differently a second time, however small the change", () => {
    const result = compare(results(1000), results(1000), results(1001), false);
    expect(result.failed).toBe(true);
    expect(result.markdown).toContain("Not reproducible");
    expect(
      compare(results(1000), results(1000), results(1000), false).failed,
    ).toBe(false);
  });

  test("passes with nothing to compare where the base has no results", () => {
    const result = compare(undefined, results(1000), results(1000), false);
    expect(result.failed).toBe(false);
    expect(result.markdown).toContain("nothing to compare");
  });

  test("marks scenarios new at the head and removed from it", () => {
    const base = results(1000);
    delete base.metrics["multi-15-zoomed"];
    base.metrics["old-scenario"] = { [instructions]: { value: 5 } };
    const result = compare(base, results(1000), undefined, false);
    expect(result.failed).toBe(false);
    expect(result.markdown).toMatch(/multi-15-zoomed .*new/);
    expect(result.markdown).toMatch(/old-scenario .*removed/);
    expect(result.markdown).not.toMatch(/compare-15-worst .*removed/);
  });
});

// The script as CI runs it: the bench's output for each build turned into results, then compared.
describe("the command", () => {
  const script = fileURLToPath(new URL("../engine-work.ts", import.meta.url));
  const run = (...args: string[]) =>
    spawnSync(process.execPath, [script, ...args], { encoding: "utf8" });

  // A build's results, from bench output: the worst case (gated) at `instructions` per frame.
  function build(root: string, name: string, perFrame: number): string {
    const dir = join(root, name);
    mkdirSync(dir);
    const bench: BenchRun = {
      scenario: "compare-15-worst",
      gated: true,
      frames: 300,
      shown: 300,
      columns: 2560,
      samplesRead: 0,
      samplesReduced: 0,
      samplesSummed: 0,
      samplesSearched: 0,
      matchSearches: 0,
    };
    writeFileSync(join(dir, "compare-15-worst.json"), JSON.stringify(bench));
    writeFileSync(
      join(dir, "compare-15-worst.cachegrind"),
      `events: Ir\nsummary: ${String(perFrame * 300)}\n`,
    );
    expect(run("results", dir).status).toBe(0);
    return dir;
  }

  test("checks one run as `results` reads it: 0 for a whole run, 1 naming what is missing", () => {
    const dir = mkdtempSync(join(tmpdir(), "engine-work-"));
    const file = join(dir, "compare-15-worst.json");
    writeFileSync(file, JSON.stringify(benchRun));
    expect(run("check", file).status).toBe(0);
    writeFileSync(file, JSON.stringify({ ...benchRun, gated: undefined }));
    const refused = run("check", file);
    expect(refused.status).toBe(1);
    expect(refused.stderr).toContain("gated");
  });

  test("fails a gated scenario's rise once its results have been written and read back", () => {
    const root = mkdtempSync(join(tmpdir(), "engine-work-"));
    const base = build(root, "base", 1000);
    const head = build(root, "head", 1100);
    const again = build(root, "again", 1100);
    const compared = run("compare", base, head, again);
    expect(compared.status).toBe(1);
    expect(compared.stdout).toContain("**fails**");
    expect(run("compare", base, head, again, "--accepted").status).toBe(0);
  });

  test("passes with nothing to compare where the base left no results", () => {
    const root = mkdtempSync(join(tmpdir(), "engine-work-"));
    const head = build(root, "head", 1000);
    const again = build(root, "again", 1000);
    const compared = run("compare", join(root, "base"), head, again);
    expect(compared.status).toBe(0);
    expect(compared.stdout).toContain("nothing to compare");
  });

  test("fails where the head, or its second count, left no results", () => {
    const root = mkdtempSync(join(tmpdir(), "engine-work-"));
    const base = build(root, "base", 1000);
    const head = build(root, "head", 1000);
    expect(run("compare", base, join(root, "head-missing"), head).status).toBe(
      1,
    );
    const unchecked = run("compare", base, head, join(root, "again-missing"));
    expect(unchecked.status).toBe(1);
    expect(unchecked.stdout).toContain("No second count");
  });
});
