import { describe, expect, test } from "vitest";

import {
  annotations,
  mullSeconds,
  relativeTo,
  summarise,
  timeLimitNote,
  warmUpSeconds,
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
    const rows = summarise(report, title)
      .split("\n")
      .filter((line) => line.startsWith("| ui/"));
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

// Mull's log, as it reads around its warm-up run: the instrumented build's mutants, the warm-up
// (the tests with no mutant), and the mutants' runs.
const mullLog = (warmUp: string) =>
  [
    "[info] Filter mutants (threads: 1)",
    "       [################################] 2307/2307. Finished in 216ms",
    "[info] Warm up run (threads: 1)",
    `       [################################] 1/1. Finished in ${warmUp}`,
    "[info] Running mutants (threads: 8)",
    "       [################################] 5/5. Finished in 1.21s",
  ].join("\n");

describe("the time limit note on a C++ mutation run", () => {
  test.each([
    ["216ms", 0.216],
    ["3151ms", 3.151],
    ["58.2s", 58.2],
    ["4m40.4s", 280.4],
    ["1h2m3s", 3723],
  ])("reads Mull's duration %s as %f s", (text, seconds) => {
    expect(mullSeconds(text)).toBeCloseTo(seconds, 6);
  });

  test.each(["", "4 minutes", "4m40.4", "ms"])(
    "reads %j as no duration",
    (text) => {
      expect(mullSeconds(text)).toBeUndefined();
    },
  );

  test.each([
    ["4m40.4s", 280.4],
    ["3151ms", 3.151],
  ])(
    "takes the warm-up's own duration (%s), not another step's",
    (warmUp, seconds) => {
      expect(warmUpSeconds(mullLog(warmUp))).toBeCloseTo(seconds, 6);
    },
  );

  test("reads the warm-up's duration with the blank line Mull's log puts before it", () => {
    const log = [
      "[warning] Could not find dynamic library: libc.so.6",
      "[info] Warm up run (threads: 1)",
      "",
      "       [################################] 1/1. Finished in 3m20.1s",
      "[info] Applying file path filter (threads: 8)",
    ].join("\n");
    expect(warmUpSeconds(log)).toBeCloseTo(200.1, 6);
  });

  test("has no warm-up time for a log without one", () => {
    expect(
      warmUpSeconds("[info] Running mutants (threads: 8)"),
    ).toBeUndefined();
  });

  // A mutant no test notices runs every test, so the tests must take well under the limit.
  test.each([
    [149, 300, false],
    [151, 300, true],
    [280, 900, false],
  ])(
    "for tests taking %d s against a %d s limit, warns: %s",
    (warmUp, limit, warns) => {
      const note = timeLimitNote(warmUp, limit);
      expect(note.split("\n")[0]).toBe(
        `The tests take ${String(warmUp)} s with no mutant; each mutant is stopped at ${String(limit)} s.`,
      );
      expect(note.includes("[!WARNING]")).toBe(warns);
    },
  );
});
