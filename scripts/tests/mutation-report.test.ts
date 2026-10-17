// The mutation report's command line, run as its callers run it: scripts/mutation.sh and
// scripts/mutation-cpp.sh for the summary, CI's Report steps for the warnings.
import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { beforeAll, describe, expect, test } from "vitest";

let dir = "";
const report = () => join(dir, "mull.json");
const log = () => join(dir, "mull.log");
const run = (...args: string[]) =>
  execFileSync(process.execPath, ["scripts/mutation-report.ts", ...args], {
    encoding: "utf8",
  });

beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), "mutation-report-"));
  writeFileSync(
    report(),
    JSON.stringify({
      files: {
        "engine/src/Roll.cpp": {
          mutants: [
            {
              mutatorName: "cxx_add_to_sub",
              replacement: "a - b",
              status: "Survived",
              location: { start: { line: 3, column: 1 } },
            },
          ],
        },
      },
    }),
  );
  writeFileSync(
    log(),
    "[info] Warm up run (threads: 1)\n       [###] 1/1. Finished in 3151ms\n",
  );
});

describe("the mutation report's command line", () => {
  test("prints a summary under its title, with the run's time (scripts/mutation.sh)", () => {
    const summary = run(report(), "Mutation testing (TypeScript)", "42");
    expect(summary.split("\n")[0]).toBe("## Mutation testing (TypeScript)");
    expect(summary).toContain("in 42 s.");
    expect(summary).not.toContain("with no mutant");
  });

  test("adds the tests' time against the limit from Mull's log (scripts/mutation-cpp.sh)", () => {
    const summary = run(
      report(),
      "Mutation testing (C++)",
      "525",
      "--mull-log",
      log(),
      "--limit",
      "300",
    );
    expect(summary).toContain(
      "The tests take 3 s with no mutant; each mutant is stopped at 300 s.",
    );
    expect(summary).not.toContain("[!WARNING]");
  });

  test("lists the mutants no test notices as JSON, for the mutation triage", () => {
    expect(JSON.parse(run(report(), "--survivors"))).toEqual([
      {
        file: "engine/src/Roll.cpp",
        line: 3,
        column: 1,
        mutator: "cxx_add_to_sub",
        replacement: "a - b",
        status: "Survived",
      },
    ]);
  });

  test("gives a removed call, which has no replacement, an empty one", () => {
    const removed = join(dir, "removed.json");
    writeFileSync(
      removed,
      JSON.stringify({
        files: {
          "engine/src/Analyzer.cpp": {
            mutants: [
              {
                mutatorName: "cxx_remove_void_call",
                status: "Survived",
                location: { start: { line: 459, column: 9 } },
              },
            ],
          },
        },
      }),
    );
    expect(JSON.parse(run(removed, "--survivors"))).toEqual([
      {
        file: "engine/src/Analyzer.cpp",
        line: 459,
        column: 9,
        mutator: "cxx_remove_void_call",
        replacement: "",
        status: "Survived",
      },
    ]);
  });

  test("prints a warning on each line no test notices (CI's Report steps)", () => {
    expect(run(report(), "--annotate").trim()).toBe(
      "::warning file=engine/src/Roll.cpp,line=3,col=1,title=Mutant survived::No test fails when this is changed (cxx_add_to_sub): a - b",
    );
  });
});

// Stryker's own log of a run in which no unit test reaches the lines it mutated (allowEmpty), as
// it writes it, colour codes included.
const noTestsLog = join(
  import.meta.dirname,
  "fixtures",
  "stryker-no-tests.log",
);

describe("a TypeScript run that wrote no report (scripts/mutation.sh)", () => {
  const missing = () => join(dir, "no-such-report.json");
  const runStatus = (...args: string[]) =>
    spawnSync(process.execPath, ["scripts/mutation-report.ts", ...args], {
      encoding: "utf8",
    });

  test("leaves a run that wrote its report summarised as usual, as mutation.sh calls it on every run", () => {
    const normal = join(dir, "stryker-normal.log");
    writeFileSync(normal, "INFO MutationTestExecutor Done in 3 seconds.\n");
    const result = runStatus(
      report(),
      "Mutation testing (TypeScript)",
      "3",
      "--stryker-log",
      normal,
      "--base",
      "origin/main",
    );
    expect(result.status).toBe(0);
    expect(result.stdout).toContain("0 of 1 mutants noticed by the tests");
    expect(result.stdout).not.toContain("No unit test reaches");
  });

  test("is summarised as nothing to judge when no unit test reaches the lines changed", () => {
    const result = runStatus(
      missing(),
      "Mutation testing (TypeScript)",
      "3",
      "--stryker-log",
      noTestsLog,
      "--base",
      "origin/main",
    );
    expect(result.status).toBe(0);
    expect(result.stdout).toContain(
      "No unit test reaches the lines changed since origin/main",
    );
  });

  test("fails when Stryker's log gives no such reason, as a run that did not complete", () => {
    const other = join(dir, "stryker-crashed.log");
    writeFileSync(
      other,
      "INFO DryRunExecutor Starting initial test run\nERROR crashed\n",
    );
    const result = runStatus(
      missing(),
      "Mutation testing (TypeScript)",
      "3",
      "--stryker-log",
      other,
      "--base",
      "origin/main",
    );
    expect(result.status).not.toBe(0);
    expect(result.stdout).toBe("");
  });
});

// Mull's own dry-run log of a run with no mutant on the lines changed.
const noMutantsLog = join(
  import.meta.dirname,
  "fixtures",
  "mull-no-mutants.log",
);

describe("a C++ dry run that wrote no list (scripts/mutation-cpp.sh)", () => {
  const missing = () => join(dir, "no-such-dry.json");
  const runStatus = (...args: string[]) =>
    spawnSync(process.execPath, ["scripts/mutation-report.ts", ...args], {
      encoding: "utf8",
    });
  const asScript = (path: string, mullLog: string) =>
    runStatus(
      path,
      "Mutation testing (C++)",
      "--mull-dry-log",
      mullLog,
      "--base",
      "origin/main",
    );

  test("is summarised as nothing to judge when no mutant is on the lines changed", () => {
    const result = asScript(missing(), noMutantsLog);
    expect(result.status).toBe(0);
    expect(result.stdout).toContain(
      "No mutant on the lines changed since origin/main",
    );
  });

  test("fails when Mull's log gives no such reason, as a run that did not complete", () => {
    const other = join(dir, "mull-crashed.log");
    writeFileSync(other, "[info] Dry run (threads: 1)\n[error] crashed\n");
    const result = asScript(missing(), other);
    expect(result.status).not.toBe(0);
    expect(result.stdout).toBe("");
  });

  test("leaves a run that wrote its list to the usual summary", () => {
    const result = asScript(report(), noMutantsLog);
    expect(result.status).toBe(0);
    expect(result.stdout).not.toContain("No mutant on the lines changed");
  });
});
