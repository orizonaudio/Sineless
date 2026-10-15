// The mutation report's command line, run as its callers run it: scripts/mutation.sh and
// scripts/mutation-cpp.sh for the summary, CI's Report steps for the warnings.
import { execFileSync } from "node:child_process";
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

  test("prints a warning on each line no test notices (CI's Report steps)", () => {
    expect(run(report(), "--annotate").trim()).toBe(
      "::warning file=engine/src/Roll.cpp,line=3,col=1,title=Mutant survived::No test fails when this is changed (cxx_add_to_sub): a - b",
    );
  });
});
