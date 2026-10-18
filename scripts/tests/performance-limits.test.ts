import { fileURLToPath } from "node:url";

import { describe, expect, test } from "vitest";

import {
  judge,
  readLimits,
  type LimitsFile,
  type Metrics,
} from "../performance-limits.ts";

const limits: LimitsFile = {
  scenarios: { "compare-15-worst": { warmup: 70, duration: 10 } },
  machines: {
    "developer-mac": {
      description: "test",
      limits: {
        "1": {
          "compare-15-worst": {
            "frames-per-second": { min: 55 },
            "late-frames-percent": { max: 2 },
          },
        },
      },
    },
  },
};

const results = (
  scale: number,
  fps: number,
  late: number | undefined,
): Metrics => ({
  "compare-15-worst": {
    "display-scale": { value: scale },
    "frames-per-second": { value: fps },
    ...(late === undefined ? {} : { "late-frames-percent": { value: late } }),
  },
});

describe("judge", () => {
  test("passes a scenario within every limit, and says so for each", () => {
    const judgement = judge(limits, "developer-mac", results(1, 59.5, 1));
    expect(judgement.failed).toBe(false);
    expect(judgement.lines).toHaveLength(2);
    expect(judgement.lines.every((line) => line.endsWith(": ok"))).toBe(true);
  });

  test("fails a measure past its limit, either way", () => {
    expect(judge(limits, "developer-mac", results(1, 50, 1)).failed).toBe(true);
    const late = judge(limits, "developer-mac", results(1, 60, 5));
    expect(late.failed).toBe(true);
    expect(late.lines).toContain(
      "limit: compare-15-worst late-frames-percent 5 <= 2: FAILED",
    );
  });

  test("fails a limited measure the run did not measure", () => {
    const judgement = judge(limits, "developer-mac", results(1, 60, undefined));
    expect(judgement.failed).toBe(true);
    expect(judgement.lines.join("\n")).toContain("not measured");
  });

  test("fails a run whose display scale was not read, on a machine with limits", () => {
    const unread = results(1, 60, 1);
    delete unread["compare-15-worst"]?.["display-scale"];
    const judgement = judge(limits, "developer-mac", unread);
    expect(judgement.failed).toBe(true);
    expect(judgement.lines.join("\n")).toContain("display scale not measured");
    expect(judge(limits, "github-macos-15", unread).failed).toBe(false);
  });

  test("measures without judging at a scale, or on a machine, with no limits", () => {
    for (const judgement of [
      judge(limits, "developer-mac", results(2, 10, 50)),
      judge(limits, "github-macos-15", results(1, 10, 50)),
    ]) {
      expect(judgement.failed).toBe(false);
      expect(judgement.lines.join("\n")).toContain("measured, not judged");
    }
  });
});

describe("performance-limits.json", () => {
  test("sets limits only for scenarios it plays, on measures perf_bench writes", () => {
    const file = readLimits(
      fileURLToPath(new URL("../performance-limits.json", import.meta.url)),
    );
    const measures = new Set([
      "frames-per-second",
      "frame-interval-median-ms",
      "frame-interval-p95-ms",
      "frame-interval-p99-ms",
      "frame-interval-max-ms",
      "late-frames-percent",
      "late-frames-worst-second",
      "tick-interval-median-ms",
      "tick-interval-p99-ms",
      "late-ticks-percent",
      "analysis-ms-per-frame",
      "main-ms-per-frame",
      "webkit-ms-per-frame",
      "total-ms-per-frame",
      "resident-mb",
    ]);
    for (const machine of Object.values(file.machines)) {
      for (const scenarios of Object.values(machine.limits)) {
        for (const [scenario, bounds] of Object.entries(scenarios)) {
          expect(Object.keys(file.scenarios)).toContain(scenario);
          for (const measure of Object.keys(bounds)) {
            expect(measures).toContain(measure);
          }
        }
      }
    }
  });
});
