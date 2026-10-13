import { describe, expect, test } from "vitest";

import {
  annotations,
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

describe("the mutation testing summary", () => {
  test("scores the mutants noticed out of those measured", () => {
    // Killed, Timeout and RuntimeError noticed; Survived and NoCoverage not; the rest left out.
    expect(summarise(report, 12)).toContain(
      "4 of 6 mutants noticed by the tests (66.7%), in 12 s.",
    );
    expect(summarise(report)).toContain("(66.7%).");
  });

  test("lists the mutants not noticed, by file and line", () => {
    const rows = summarise(report)
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
    const rows = summarise(unordered)
      .split("\n")
      .filter((line) => line.startsWith("| a.ts"));
    expect(rows.map((row) => row.split(" ")[1])).toEqual(["a.ts:2", "a.ts:9"]);
  });

  test("keeps a change on one table row", () => {
    const multiline: Report = {
      files: { "a.ts": { mutants: [mutant("Survived", 1, "a ||\n  b")] } },
    };
    expect(summarise(multiline)).toContain("`a \\|\\| b`");
  });

  test("says when there is nothing to measure", () => {
    expect(summarise({ files: {} })).toContain("No mutants to test.");
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
