// The rules that decide the mutation-triage check: a mutant the triage did not judge, and a gap no
// verifier confirmed, must never let the check pass; a gap the verifier rejects does not fail it.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import {
  comment,
  gapsLeft,
  gapsToVerify,
  judge,
  runOutcome,
  verify,
  type Mutant,
} from "../mutation-triage.ts";

const mutant = (id: number, replacement = "<="): Mutant => ({
  id,
  file: "engine/src/Analyzer.cpp",
  line: 100 + id,
  mutator: replacement === "" ? "cxx_remove_void_call" : "cxx_lt_to_le",
  replacement,
});

describe("the mutation triage's verdicts", () => {
  test("counts a mutant the triage left without a verdict as a gap", () => {
    const judged = judge(
      [mutant(0), mutant(1)],
      [{ id: 0, verdict: "equivalent", reason: "never meet" }],
    );
    expect(judged.map((m) => m.verdict)).toEqual(["equivalent", "gap"]);
    expect(gapsToVerify(judged).map((gap) => gap.id)).toEqual([1]);
  });

  test.each([
    ["no verification", [], "gap", 1],
    [
      "a confirmed gap",
      [{ id: 0, verified: true, severity: "important", proof: "p" }],
      "gap",
      1,
    ],
    [
      "a rejected gap",
      [{ id: 0, verified: false, severity: "important", proof: "p" }],
      "not a gap",
      0,
    ],
    [
      "a gap rated a nit",
      [{ id: 0, verified: true, severity: "nit", proof: "p" }],
      "not a gap",
      0,
    ],
  ] as const)(
    "after %s, the mutant is %j and %d gap(s) fail the check",
    (_, verifications, verdict, left) => {
      const judged = judge(
        [mutant(0)],
        [{ id: 0, verdict: "gap", reason: "r" }],
      );
      const result = verify(judged, [...verifications]);
      expect(result[0]?.verdict).toBe(verdict);
      expect(gapsLeft(result)).toBe(left);
    },
  );

  test("leaves a mutant judged other than a gap as it is", () => {
    const judged = judge(
      [mutant(0)],
      [{ id: 0, verdict: "tuned", reason: "a gate" }],
    );
    expect(verify(judged, [])[0]?.verdict).toBe("tuned");
    expect(gapsLeft(verify(judged, []))).toBe(0);
  });
});

describe("what a mutation job left to triage", () => {
  test.each([
    ["did not run", "skipped", false, undefined, "none"],
    ["is absent from the run", "", false, undefined, "none"],
    [
      "changed no TypeScript line",
      "success",
      false,
      "## Mutation testing (TypeScript)\n\nNo TypeScript source lines changed since origin/main.\n",
      "none",
    ],
    [
      "changed no engine line",
      "success",
      false,
      "## Mutation testing (C++)\n\nNo engine source lines changed since origin/main.\n",
      "none",
    ],
    [
      "left a report",
      "success",
      true,
      "## Mutation testing (C++)\n\n3 of 4 mutants noticed",
      "report",
    ],
    [
      "stopped after 75 minutes",
      "success",
      false,
      "## Mutation testing (C++)\n\nStopped after 75 minutes: too many mutants on the lines changed since origin/main.\n",
      "incomplete",
    ],
    [
      "failed before writing anything",
      "success",
      false,
      undefined,
      "incomplete",
    ],
    ["timed out", "cancelled", false, undefined, "incomplete"],
    ["failed", "failure", false, undefined, "incomplete"],
  ] as const)(
    "a job that %s (%s) leaves the outcome its run allows",
    (_, conclusion, hasReport, summary, outcome) => {
      expect(runOutcome(conclusion, hasReport, summary)).toBe(outcome);
    },
  );
});

// The summary each mutation script writes when no line it mutates changed, read from the script
// itself, so that the test follows the script's wording.
function nothingChangedSummary(script: string): string {
  const match =
    /printf '(## Mutation testing[^']*No [^']*source lines changed since %s\.\\n)'/u.exec(
      readFileSync(script, "utf8"),
    );
  expect(match, script).not.toBeNull();
  return (match?.[1] ?? "")
    .replaceAll("\\n", "\n")
    .replace("%s", "origin/main");
}

describe("a pull request that changes no line a mutation job mutates", () => {
  test.each(["scripts/mutation.sh", "scripts/mutation-cpp.sh"])(
    "is not incomplete for %s's summary",
    (script) => {
      expect(runOutcome("success", false, nothingChangedSummary(script))).toBe(
        "none",
      );
    },
  );
});

describe("the mutation triage's comment", () => {
  test("says the check failed, and lists each gap with its verification", () => {
    const result = verify(
      judge(
        [mutant(0), mutant(1)],
        [
          { id: 0, verdict: "gap", reason: "a frozen view redraws" },
          { id: 1, verdict: "equivalent", reason: "never meet" },
        ],
      ),
      [{ id: 0, verified: true, severity: "important", proof: "traced" }],
    );
    const text = comment(result);
    expect(text.split("\n").slice(0, 2)).toEqual([
      "<!-- mutation-triage: failed -->",
      "### Mutation triage: 1 untested gap(s)",
    ]);
    expect(text).toContain("- **Gap** `engine/src/Analyzer.cpp:100`");
    expect(text).toContain("Verification: traced");
    expect(text).toContain("| equivalent | never meet |");
  });

  test("says it passed when no gap is left", () => {
    const result = verify(
      judge([mutant(0)], [{ id: 0, verdict: "dead", reason: "cannot run" }]),
      [],
    );
    expect(comment(result).split("\n")[0]).toBe(
      "<!-- mutation-triage: passed -->",
    );
  });

  test("names a removed call as removed, not as an empty change", () => {
    const judged = judge(
      [mutant(0, "")],
      [{ id: 0, verdict: "gap", reason: "r" }],
    );
    expect(gapsToVerify(judged)[0]?.title).toContain(
      "No test notices the call removed",
    );
    expect(comment(judged)).toContain("cxx_remove_void_call: removed");
  });
});

describe("the mutation triage's command line, as the workflow runs it", () => {
  test.each([
    ["a report", true, undefined, "report"],
    [
      "a summary that nothing changed",
      false,
      "scripts/mutation-cpp.sh",
      "none",
    ],
    ["neither", false, undefined, "incomplete"],
  ] as const)(
    "says what a finished job with %s left",
    (_, hasReport, summaryFrom, outcome) => {
      const dir = mkdtempSync(join(tmpdir(), "mutation-outcome-"));
      const report = join(dir, "mull.json");
      const summary = join(dir, "summary.md");
      if (hasReport) {
        writeFileSync(report, '{"files":{}}');
      }
      if (summaryFrom !== undefined) {
        writeFileSync(summary, nothingChangedSummary(summaryFrom));
      }
      const printed = execFileSync(
        process.execPath,
        ["scripts/mutation-triage.ts", "outcome", "success", report, summary],
        { encoding: "utf8" },
      );
      expect(printed.trim()).toBe(outcome);
    },
  );

  test("judges, then reports the gaps left and the comment", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-triage-"));
    const file = (name: string, value: unknown) => {
      writeFileSync(join(dir, name), JSON.stringify(value));
      return join(dir, name);
    };
    const run = (...args: string[]) =>
      JSON.parse(
        execFileSync(
          process.execPath,
          ["scripts/mutation-triage.ts", ...args],
          {
            encoding: "utf8",
          },
        ),
      ) as Record<string, unknown>;
    const judged = run(
      "judge",
      file("mutants.json", [mutant(0), mutant(1)]),
      file("triage.json", {
        verdicts: [{ id: 0, verdict: "gap", reason: "r" }],
      }),
    );
    expect((judged.gaps as unknown[]).length).toBe(2);
    const report = run(
      "report",
      file("judged.json", judged.judged),
      file("verify.json", {
        verdicts: [
          { id: 0, verified: false, severity: "nit", proof: "redone later" },
          { id: 1, verified: true, severity: "important", proof: "traced" },
        ],
      }),
    );
    expect(report.gaps).toBe(1);
    expect(report.total).toBe(2);
    expect(report.comment).toContain("### Mutation triage: 1 untested gap(s)");
  });
});
