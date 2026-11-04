import { readFileSync } from "node:fs";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import type { BaselineReport } from "../mutation-run.ts";
import {
  annotations,
  baselineNote,
  noMutantFound,
  noTestReached,
  notRunCount,
  relativeTo,
  summarise,
  type Mutant,
  type Report,
} from "../mutation-summary.ts";

const mutant = (
  status: string,
  line: number,
  replacement = "true",
): Mutant => ({
  mutatorName: "ConditionalExpression",
  replacement,
  status,
  location: { start: { line, column: 3 } },
});

const report: Report = {
  files: {
    "ui/src/b.ts": {
      mutants: [mutant("Survived", 9), mutant("Killed", 1)],
    },
    "ui/src/a.ts": {
      mutants: [
        mutant("Killed", 2),
        mutant("Timeout", 3),
        mutant("RuntimeError", 4),
        mutant("NoCoverage", 7),
        mutant("CompileError", 5),
        mutant("Ignored", 6),
      ],
    },
  },
};

const title = "Mutation testing (C++)";

describe("the mutation testing summary", () => {
  test("is headed by its title", () => {
    expect(summarise(report, title).split("\n")[0]).toBe(
      "## Mutation testing (C++)",
    );
  });

  test("names files from the repository, as Mull's absolute paths are not", () => {
    const absolute: Report = {
      files: {
        "/work/repo/engine/src/a.cpp": { mutants: [mutant("Survived", 4)] },
        "ui/src/b.ts": { mutants: [mutant("Survived", 5)] },
      },
    };
    expect(relativeTo(absolute, "/work/repo")).toEqual({
      files: {
        "engine/src/a.cpp": { mutants: [mutant("Survived", 4)] },
        "ui/src/b.ts": { mutants: [mutant("Survived", 5)] },
      },
    });
  });

  test("scores the mutants noticed out of those measured", () => {
    // Killed, Timeout and RuntimeError noticed; Survived and NoCoverage not; the rest left out.
    expect(summarise(report, title, 12)).toContain(
      "4 of 6 mutants noticed by the tests (66.7%), in 12 s.",
    );
    expect(summarise(report, title)).toContain("(66.7%).");
  });

  test("lists the mutants not noticed, by file and line", () => {
    // The not-noticed table's rows, three cells each; the timed-out table's have two.
    const rows = summarise(report, title)
      .split("\n")
      .filter(
        (line) => line.startsWith("| ui/") && line.split(" | ").length === 3,
      );
    expect(rows).toEqual([
      "| ui/src/a.ts:7 | NoCoverage | ConditionalExpression: `true` |",
      "| ui/src/b.ts:9 | Survived | ConditionalExpression: `true` |",
    ]);
  });

  test("lists a file's mutants by line", () => {
    const unordered: Report = {
      files: {
        "a.ts": { mutants: [mutant("Survived", 9), mutant("Survived", 2)] },
      },
    };
    const rows = summarise(unordered, title)
      .split("\n")
      .filter((line) => line.startsWith("| a.ts"));
    expect(rows.map((row) => row.split(" ")[1])).toEqual(["a.ts:2", "a.ts:9"]);
  });

  test("keeps a change on one table row", () => {
    const multiline: Report = {
      files: { "a.ts": { mutants: [mutant("Survived", 1, "a ||\n  b")] } },
    };
    expect(summarise(multiline, title)).toContain("`a \\|\\| b`");
  });

  test("says when there is nothing to measure", () => {
    expect(summarise({ files: {} }, title)).toContain("No mutants to test.");
  });

  test("says a run that stopped at its deadline is partial, and how many mutants it did not run", () => {
    const partial: Report = {
      files: {
        "a.cpp": {
          mutants: [
            mutant("Killed", 1),
            mutant("Pending", 2),
            mutant("Pending", 3),
          ],
        },
      },
    };
    const text = summarise(partial, title);
    expect(text).toContain("1 of 1 mutants noticed");
    expect(text).toContain("**Partial:** 2 mutants were not run");
    expect(notRunCount(partial)).toBe(2);
    expect(summarise(report, title)).not.toContain("Partial");
  });

  test("does not call a run that ran no mutant one with nothing to test", () => {
    const none: Report = {
      files: { "a.cpp": { mutants: [mutant("Pending", 2)] } },
    };
    const text = summarise(none, title);
    expect(text).not.toContain("No mutants to test.");
    expect(text).toContain("No mutant was run.");
    expect(text).toContain("**Partial:** 1 mutants were not run");
  });
});

describe("the mutation testing warnings", () => {
  test("mark each mutant not noticed on its line", () => {
    expect(annotations(report)).toEqual([
      "::warning file=ui/src/a.ts,line=7,col=3,title=Mutant nocoverage::No test fails when this is changed (ConditionalExpression): true",
      "::warning file=ui/src/b.ts,line=9,col=3,title=Mutant survived::No test fails when this is changed (ConditionalExpression): true",
    ]);
  });

  test("escape what GitHub would decode", () => {
    const modulo: Report = {
      files: { "a,b:c.ts": { mutants: [mutant("Survived", 1, "a % b")] } },
    };
    const [warning] = annotations(modulo);
    expect(warning).toContain("file=a%2Cb%3Ac.ts,");
    expect(warning).toMatch(/: a %25 b$/u);
  });

  test("stop at the 10 GitHub shows", () => {
    const many: Report = {
      files: {
        "a.ts": {
          mutants: Array.from({ length: 12 }, (_, line) =>
            mutant("Survived", line + 1),
          ),
        },
      },
    };
    expect(annotations(many)).toHaveLength(10);
  });
});

// What two shares' runs with no mutant measured (scripts/mutation-run.ts, --baseline-out).
const share = (engine: number, plugin?: number): BaselineReport => ({
  factor: 1.5,
  constantSeconds: 5,
  binaries: [
    {
      name: "engine_tests",
      tests: 3344,
      seconds: engine,
      limitSeconds: 1.5 * engine + 5,
    },
    ...(plugin === undefined
      ? []
      : [
          {
            name: "plugin_tests",
            tests: 40,
            seconds: plugin,
            limitSeconds: 1.5 * plugin + 5,
          },
        ]),
  ],
});

describe("the C++ runs' times with no mutant", () => {
  test("gives each module the largest time any share measured, and where its mutants are stopped", () => {
    const note = baselineNote(
      [share(700, 12), share(812)],
      ["engine_tests", "plugin_tests"],
    );
    expect(note).toContain("| engine_tests | 812 s | 1223 s |");
    expect(note).toContain("| plugin_tests | 12 s | 23 s |");
    expect(note.split("\n")[0]).toBe(
      "Each mutant is stopped at 1.5 × its tests' time with no mutant + 5 s, the time measured with every worker busy.",
    );
  });

  // A share with no mutant of a module, or whose run with no mutant failed, writes no times for it:
  // the module then shows no time rather than 0 s, which would read as tests that take none.
  test("shows no time for a module no share timed, rather than 0 s", () => {
    const note = baselineNote([share(812)], ["engine_tests", "editor_tests"]);
    expect(note).toContain("| editor_tests | — | — |");
    expect(note).not.toMatch(/\| 0 s \|/u);
  });

  test("shows each module with mutants as untimed when no share wrote its times", () => {
    expect(baselineNote([], ["engine_tests"])).toBe(
      [
        "| Tests | With no mutant | Each mutant stopped at |",
        "| --- | ---: | ---: |",
        "| engine_tests | — | — |",
      ].join("\n"),
    );
  });

  test("says nothing when no module had mutants", () => {
    expect(baselineNote([], [])).toBe("");
  });

  test("no longer warns about MUTANT_TIMEOUT, which no run reads", () => {
    expect(baselineNote([share(1812)], ["engine_tests"])).not.toContain(
      "MUTANT_TIMEOUT",
    );
  });
});

describe("the timed-out mutants", () => {
  // A timeout counts as noticed; listed so that a survivor a limit set too tight stopped shows.
  test("lists each mutant that timed out, by file and line", () => {
    const summary = summarise(report, title);
    expect(summary).toContain("Timed out: each hung its tests past its limit");
    expect(summary).toContain(
      "| ui/src/a.ts:3 | ConditionalExpression: `true` |",
    );
  });

  test("lists none when no mutant timed out", () => {
    expect(
      summarise(
        { files: { "a.cpp": { mutants: [mutant("Killed", 1)] } } },
        title,
      ),
    ).not.toContain("Timed out");
  });
});

describe("a Stryker run no unit test reaches", () => {
  // Stryker's own log of such a run (allowEmpty), colour codes included.
  const log = readFileSync(
    join(import.meta.dirname, "fixtures", "stryker-no-tests.log"),
    "utf8",
  );

  test("is told from its log when it wrote no report", () => {
    expect(noTestReached(log, false)).toBe(true);
  });

  test("is not when it wrote a report, or its log gives another reason", () => {
    expect(noTestReached(log, true)).toBe(false);
    expect(
      noTestReached("INFO DryRunExecutor Starting initial test run\n", false),
    ).toBe(false);
  });
});

describe("a Mull dry run with no mutant on the lines changed", () => {
  // Mull's own log of such a run, from a pull request that changed only a comment of the engine.
  const log = readFileSync(
    join(import.meta.dirname, "fixtures", "mull-no-mutants.log"),
    "utf8",
  );

  test("is told from its log", () => {
    expect(noMutantFound(log)).toBe(true);
  });

  test("is not when the log gives another reason for no list", () => {
    expect(
      noMutantFound("[info] Dry run (threads: 1)\n[error] crashed\n"),
    ).toBe(false);
  });
});
