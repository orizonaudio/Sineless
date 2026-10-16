// The rules that decide the mutation-triage check: a mutant the triage did not judge, and a gap no
// verifier confirmed, must never let the check pass; a gap the verifier rejects does not fail it.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import {
  checkState,
  comment,
  gapsLeft,
  gapsToVerify,
  judge,
  numbered,
  repositoryPath,
  runOutcome,
  verdicts,
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
  test("are the ones the workflow lets the triage return, no more and no fewer", () => {
    const workflow = readFileSync(
      join(import.meta.dirname, "../../.github/workflows/mutation-triage.yml"),
      "utf8",
    );
    const allowed = /"verdict":\{"enum":(\[[^\]]*\])\}/u.exec(workflow)?.[1];
    expect(JSON.parse(allowed ?? "null")).toEqual([...verdicts]);
  });

  test.each(verdicts.filter((verdict) => verdict !== "gap"))(
    "never sends a mutant judged %s to a verifier, nor fails the check on it",
    (verdict) => {
      const judged = judge([mutant(0)], [{ id: 0, verdict, reason: "r" }]);
      expect(gapsToVerify(judged)).toEqual([]);
      expect(gapsLeft(verify(judged, []))).toBe(0);
    },
  );

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
      "wrote only a summary that is not about an unchanged engine",
      "success",
      false,
      "## Mutation testing (C++)\n\nMutation testing did not run.\n",
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

  test("a report with mutants not run is partial, never a full report", () => {
    expect(runOutcome("success", true, undefined, 3)).toBe("partial");
    expect(runOutcome("failure", true, undefined, 1)).toBe("partial");
  });
});

describe("the mutation triage's state", () => {
  test.each([
    [0, 0, 0, "success"],
    [0, 5, 0, "success"],
    [1, 5, 0, "failure"],
    [1, 5, 2, "failure"],
    [0, 5, 2, "error"],
    [0, 0, 2, "error"],
  ] as const)(
    "%d gap(s) among %d mutant(s), %d not run: %s",
    (gaps, total, notRun, state) => {
      expect(checkState(gaps, total, notRun).state).toBe(state);
    },
  );

  test("says in its description how many mutants were not run", () => {
    expect(checkState(0, 5, 1).description).toContain("1 not run");
  });
});

// Every summary a mutation script writes when it has nothing to report (no line it mutates
// changed, or no test reaches them), read from the script itself, as it prints it against
// origin/main, so that the test follows the script's wording.
function nothingToJudgeSummaries(script: string): string[] {
  const summaries = [
    ...readFileSync(script, "utf8").matchAll(
      /printf '(## Mutation testing[^']*since %s[^']*)'/gu,
    ),
  ].map((match) =>
    (match[1] ?? "").replaceAll("\\n", "\n").replace("%s", "origin/main"),
  );
  expect(summaries.length, script).toBeGreaterThan(0);
  return summaries;
}

function nothingChangedSummary(script: string): string {
  return nothingToJudgeSummaries(script)[0] ?? "";
}

describe("the reports the workflow triages", () => {
  // Each report the collect step reads, as report artifact/file, must be downloaded by its name
  // into its own folder: downloaded by pattern, a lone report lands straight in reports/, and a
  // job that finished would read as incomplete.
  const workflow = readFileSync(
    join(import.meta.dirname, "../../.github/workflows/mutation-triage.yml"),
    "utf8",
  );
  const read = [
    ...(/for kind in ([^;]+); do/u.exec(workflow)?.[1] ?? "").matchAll(
      /\S+:([^/\s]+)\/\S+/gu,
    ),
  ].map((match) => match[1] ?? "");

  test("are the two mutation jobs'", () => {
    expect(read).toEqual(["mutation-ts", "mutation-cpp"]);
  });

  test.each(read)(
    "%s is downloaded by its name into the folder the collect step reads",
    (artifact) => {
      expect(workflow).toContain(
        `name: ${artifact}\n          path: mutation-triage/reports/${artifact}\n`,
      );
      expect(workflow).not.toMatch(/pattern: mutation-/u);
    },
  );
});

describe("a pull request that changes no line a mutation job mutates or tests", () => {
  test.each(["scripts/mutation.sh", "scripts/mutation-cpp.sh"])(
    "is not incomplete for any of %s's summaries with nothing to judge",
    (script) => {
      for (const summary of nothingToJudgeSummaries(script)) {
        expect(runOutcome("success", false, summary), summary).toBe("none");
      }
    },
  );
});

describe("the mutation triage's comment", () => {
  const gapsListed = (text: string) =>
    text.split("\n").filter((line) => line.startsWith("- **Gap**"));

  test("lists exactly the confirmed gaps, each with its verification, and asks for tests", () => {
    const result = verify(
      judge(
        [mutant(0), mutant(1), mutant(2)],
        [
          { id: 0, verdict: "gap", reason: "a frozen view redraws" },
          { id: 1, verdict: "gap", reason: "r" },
          { id: 2, verdict: "equivalent", reason: "never meet" },
        ],
      ),
      [
        { id: 0, verified: true, severity: "important", proof: "traced" },
        {
          id: 1,
          verified: false,
          severity: "important",
          proof: "redone later",
        },
      ],
    );
    const text = comment(result);
    expect(gapsListed(text)).toHaveLength(1);
    expect(gapsListed(text)[0]).toContain("engine/src/Analyzer.cpp:100");
    expect(text).toContain("Verification: traced");
    expect(text).toContain("Add a test");
    // Every mutant is still listed, each with its verdict and why.
    expect(text).toContain("| not a gap | redone later |");
    expect(text).toContain("| equivalent | never meet |");
  });

  test("lists no gap and asks for no test when none is left", () => {
    const result = verify(
      judge([mutant(0)], [{ id: 0, verdict: "gap", reason: "r" }]),
      [{ id: 0, verified: false, severity: "nit", proof: "harmless" }],
    );
    const text = comment(result);
    expect(gapsListed(text)).toHaveLength(0);
    expect(text).not.toContain("Add a test");
  });

  test("says a partial run cannot pass, and how many mutants it did not run", () => {
    const result = verify(
      judge([mutant(0)], [{ id: 0, verdict: "dead", reason: "cannot run" }]),
      [],
    );
    const text = comment(result, 4);
    expect(text.split("\n")[0]).toBe("<!-- mutation-triage: partial -->");
    expect(text).toContain("**Partial:** 4 mutant(s) were not run");
    expect(comment(result)).not.toContain("Partial");
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

describe("the paths the triagers read", () => {
  test.each([
    ["engine/src/Analyzer.cpp", "engine/src/Analyzer.cpp"],
    ["ui/src/app.ts", "ui/src/app.ts"],
    ["/src/engine/src/Roll.cpp", "engine/src/Roll.cpp"],
    [
      "/home/runner/work/Sineless/Sineless/plugin/src/Processor.cpp",
      "plugin/src/Processor.cpp",
    ],
    ["../../work/Sineless/protocol/src/frame.ts", "protocol/src/frame.ts"],
    ["/x/engine/src/engine/Nested.cpp", "engine/src/engine/Nested.cpp"],
  ])("are %s as %s, a path in the repository", (from, to) => {
    expect(repositoryPath(from)).toBe(to);
  });

  test("number every mutant of every report, from 0", () => {
    const first = {
      file: "engine/src/Analyzer.cpp",
      line: 105,
      mutator: "m",
      replacement: "x",
    };
    expect(
      numbered([first, { ...first, file: "/src/ui/src/app.ts" }]).map((m) => [
        m.id,
        m.file,
      ]),
    ).toEqual([
      [0, "engine/src/Analyzer.cpp"],
      [1, "ui/src/app.ts"],
    ]);
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

  test("never passes a report with mutants not run, though no gap is left", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-partial-"));
    const report = join(dir, "mull.json");
    const reportMutant = (status: string, line: number) => ({
      id: `m${String(line)}`,
      mutatorName: "cxx_lt_to_le",
      replacement: "<=",
      location: { start: { line, column: 1 } },
      status,
    });
    writeFileSync(
      report,
      JSON.stringify({
        files: {
          "engine/src/Roll.cpp": {
            mutants: [
              reportMutant("Killed", 1),
              reportMutant("Pending", 2),
              reportMutant("Pending", 3),
            ],
          },
        },
      }),
    );
    const cli = (...args: string[]) =>
      execFileSync(process.execPath, ["scripts/mutation-triage.ts", ...args], {
        encoding: "utf8",
      }).trim();
    expect(cli("outcome", "success", report)).toBe("partial");
    const notRun = cli("not-run", report);
    expect(notRun).toBe("2");
    const judged = join(dir, "judged.json");
    const verify = join(dir, "verify.json");
    writeFileSync(judged, "[]");
    writeFileSync(verify, '{"verdicts":[]}');
    const result = JSON.parse(cli("report", judged, verify, notRun)) as {
      state: string;
      comment: string;
    };
    expect(result.state).not.toBe("success");
    expect(result.comment).toContain("2 mutant(s) were not run");
  });
});
