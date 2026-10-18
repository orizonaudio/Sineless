// Running the C++ mutants a share at a time: each share gets some of every file, each mutant runs
// against its own module's tests, its result is kept as soon as it is known, a mutant that hangs
// is stopped at its module's limit, and the mutants left at the deadline are reported as not run
// rather than lost.
import {
  chmodSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import { execFileSync, spawnSync } from "node:child_process";

import {
  combine,
  merge,
  missingSwitches,
  mutantFiles,
  mutantIds,
  run,
  shareOf,
  statusOf,
  testsFor,
  testsPass,
  type Tests,
} from "../mutation-run.ts";
import type { Report } from "../mutation-summary.ts";

const mutant = (id: string) => ({
  id,
  mutatorName: id.split(":")[0] ?? "",
  replacement: "x",
  location: { start: { line: 1, column: 1 } },
  status: "DryRun",
});

const dryRun: Report = {
  files: {
    "engine/src/A.cpp": {
      mutants: [mutant("kill:a1"), mutant("survive:a2"), mutant("kill:a3")],
    },
    "engine/src/B.cpp": { mutants: [mutant("survive:b1")] },
  },
};

const fake = join(import.meta.dirname, "fixtures", "fake-tests.mjs");
// The fake tests, stopped at `limitSeconds`.
const fakeTests = (limitSeconds: number): Tests => ({
  command: process.execPath,
  args: [fake],
  limitSeconds,
});
const withTests = (ids: string[], tests: Tests) =>
  ids.map((id) => ({ id, tests }));

describe("sharing out the mutants", () => {
  test("gives every mutant to exactly one share, whatever the number of shares", () => {
    const ids = mutantIds(dryRun);
    for (const shares of [1, 2, 3, 4, 5]) {
      const all = Array.from({ length: shares }, (_, i) =>
        shareOf(ids, i + 1, shares),
      ).flat();
      expect(all.sort()).toEqual(ids);
    }
  });

  test("shares one large file's mutants out, rather than giving it to one share", () => {
    const ids = mutantIds(dryRun);
    const firstFile = (share: string[]) =>
      share.filter(
        (id) => id.endsWith("a1") || id.endsWith("a2") || id.endsWith("a3"),
      );
    expect(firstFile(shareOf(ids, 1, 2)).length).toBeGreaterThan(0);
    expect(firstFile(shareOf(ids, 2, 2)).length).toBeGreaterThan(0);
  });

  test.each([
    [0, 2],
    [3, 2],
    [1, 0],
  ])("refuses share %d of %d", (share, shares) => {
    expect(() => shareOf(["x"], share, shares)).toThrow();
  });
});

describe("a mutant's result", () => {
  test.each([
    [1, false, "Killed"],
    [0, false, "Survived"],
    [null, true, "Timeout"],
    [0, true, "Timeout"],
  ] as const)("exit %s, stopped %s: %s", (code, timedOut, status) => {
    expect(statusOf(code, timedOut)).toBe(status);
  });
});

describe("choosing each mutant's tests", () => {
  const engine = fakeTests(900);
  const plugin = fakeTests(60);
  const editor = fakeTests(60);
  const tests = new Map([
    ["engine", engine],
    ["plugin", plugin],
    ["editor", editor],
  ]);

  test.each([
    ["engine/src/Analyzer.cpp", engine],
    ["/src/engine/include/engine/CaptureRing.h", engine],
    ["/home/runner/work/Sineless/Sineless/plugin/src/Processor.cpp", plugin],
    ["/src/editor/src/Session.cpp", editor],
    ["/src/editor/include/editor/Session.h", editor],
    // A checkout whose path names another module still runs a file's mutants against its own.
    ["/work/engine/Sineless/editor/src/Session.cpp", editor],
  ])("runs %s's mutants against its own module's tests", (file, expected) => {
    expect(testsFor(file, tests)).toBe(expected);
  });

  test.each([
    "/src/protocol/src/limits.ts",
    "/src/plugin/tests/StateTests.cpp",
  ])("gives %s no tests, as no module's sources hold it", (file) => {
    expect(testsFor(file, tests)).toBeUndefined();
  });

  test("knows each mutant's file from the dry run", () => {
    expect(mutantFiles(dryRun).get("survive:b1")).toBe("engine/src/B.cpp");
  });
});

describe("running a share", () => {
  test("runs each mutant against its own tests, stopped at their limit, not another module's", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    // The plugin's tests pass whatever the mutant; the engine's are the fake ones, with a limit
    // the test would outlast.
    const passing: Tests = {
      command: process.execPath,
      args: ["-e", "process.exit(0)"],
      limitSeconds: 60,
    };
    const done = await run({
      mutants: [
        { id: "kill:engine", tests: fakeTests(60) },
        { id: "kill:plugin", tests: passing },
        { id: "hang:editor", tests: fakeTests(1) },
      ],
      results,
      deadlineSeconds: 60,
      workers: 3,
    });
    expect(Object.fromEntries(done.map((r) => [r.id, r.status]))).toEqual({
      "kill:engine": "Killed",
      "kill:plugin": "Survived",
      "hang:editor": "Timeout",
    });
  });

  test("writes each result as soon as it is known, so a share cut short keeps what it ran", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    let finished = false;
    const running = run({
      mutants: withTests(["kill:a", "hang:b"], fakeTests(3)),
      results,
      deadlineSeconds: 60,
      workers: 2,
    }).then((done) => {
      finished = true;
      return done;
    });
    const written = () =>
      existsSync(results) ? readFileSync(results, "utf8") : "";
    await expect.poll(written, { timeout: 2000 }).toContain('"kill:a"');
    expect(finished).toBe(false);
    expect(written()).not.toContain('"hang:b"');
    await running;
  });

  test("keeps each result as it comes, and stops a mutant that hangs at the limit", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      mutants: withTests(["kill:a", "survive:b", "hang:c"], fakeTests(1)),
      results,
      deadlineSeconds: 60,
      workers: 3,
    });
    const byId = Object.fromEntries(done.map((r) => [r.id, r.status]));
    expect(byId).toEqual({
      "kill:a": "Killed",
      "survive:b": "Survived",
      "hang:c": "Timeout",
    });
    const written = readFileSync(results, "utf8").trim().split("\n");
    expect(written).toHaveLength(3);
  });

  test("reports the mutants left at the deadline as not run, not as lost", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      mutants: withTests(["hang:a", "kill:b", "kill:c"], fakeTests(1)),
      results,
      deadlineSeconds: 0.5,
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["Timeout", "NotRun", "NotRun"]);
    expect(readFileSync(results, "utf8").trim().split("\n")).toHaveLength(3);
  });
});

describe("listing the mutants of several test binaries", () => {
  test("lists every module's mutants together, so that each share gets some of each", () => {
    const plugin: Report = {
      files: { "plugin/src/Processor.cpp": { mutants: [mutant("kill:p1")] } },
    };
    expect(mutantIds(combine([dryRun, plugin]))).toEqual(
      [...mutantIds(dryRun), "kill:p1"].sort(),
    );
  });

  test("lists no mutant from no dry run", () => {
    expect(mutantIds(combine([]))).toEqual([]);
  });
});

describe("merging the shares' results", () => {
  test("reports every mutant as not run when no share left a results file", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    // As scripts/mutation-cpp.sh passes it when no results-*.jsonl exists: the pattern itself.
    const merged = JSON.parse(
      execFileSync(
        process.execPath,
        [script, "merge", dry, join(dir, "results-*.jsonl")],
        { encoding: "utf8" },
      ),
    ) as Report;
    const statuses = Object.values(merged.files).flatMap(({ mutants }) =>
      mutants.map((m) => m.status),
    );
    expect(statuses).toEqual(["Pending", "Pending", "Pending", "Pending"]);
  });

  test("gives each mutant its result, and one not run the report format's Pending", () => {
    const merged = merge(dryRun, [
      { id: "kill:a1", status: "Killed" },
      { id: "survive:a2", status: "Survived" },
      { id: "kill:a3", status: "NotRun" },
    ]);
    const statuses = Object.values(merged.files).flatMap(({ mutants }) =>
      mutants.map((m) => [m.id, m.status]),
    );
    expect(statuses).toEqual([
      ["kill:a1", "Killed"],
      ["survive:a2", "Survived"],
      ["kill:a3", "Pending"],
      ["survive:b1", "Pending"],
    ]);
  });
});

describe("checking the test binary can switch each mutant on", () => {
  test("finds each mutant's switch, named after its id, in the binary", () => {
    const binary = Buffer.from("\0code\0kill:a1\0survive:a2\0more");
    expect(missingSwitches(binary, ["kill:a1", "survive:a2"])).toEqual([]);
  });

  test("names the mutants whose switch is missing, a prefix of another's included", () => {
    const binary = Buffer.from("\0kill:a10\0");
    expect(missingSwitches(binary, ["kill:a1", "kill:a10"])).toEqual([
      "kill:a1",
    ]);
  });

  test("checks each module's test binary for its own mutants' switches only, as each holds no other's", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const plugin: Report = {
      files: { "plugin/src/C.cpp": { mutants: [mutant("survive:c1")] } },
    };
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(combine([dryRun, plugin])));
    // Each passes whatever the mutant, and holds the switches of its own module's mutants only.
    const binary = (name: string, report: Report) => {
      const path = join(dir, name);
      writeFileSync(
        path,
        `#!/bin/sh\nexit 0\n# ${mutantIds(report).join("\0")}\0\n`,
      );
      chmodSync(path, 0o755);
      return path;
    };
    const results = join(dir, "results.jsonl");
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    execFileSync(
      process.execPath,
      [
        script,
        "run",
        dry,
        "1/1",
        results,
        "--deadline",
        "5",
        "--tests",
        "engine",
        "5",
        binary("engine_tests", dryRun),
        "--tests",
        "plugin",
        "5",
        binary("plugin_tests", plugin),
      ],
      { stdio: "pipe" },
    );
    expect(readFileSync(results, "utf8").trim().split("\n")).toHaveLength(5);
  });

  test("refuses to run a share whose mutants the binary cannot switch on, as all would go unnoticed", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    const binary = join(dir, "engine_tests");
    writeFileSync(binary, "no switches here");
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    expect(() =>
      execFileSync(
        process.execPath,
        [
          script,
          "run",
          dry,
          "1/1",
          join(dir, "results.jsonl"),
          "--deadline",
          "1",
          "--tests",
          "engine",
          "1",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/no switch for 4 of the 4 mutants/u);
  });
});

describe("checking each mutant has tests to run against", () => {
  test("refuses to run a share with mutants of a module given no tests, rather than skip them", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(
      dry,
      JSON.stringify(
        combine([
          dryRun,
          { files: { "plugin/src/C.cpp": { mutants: [mutant("kill:c1")] } } },
        ]),
      ),
    );
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    expect(() =>
      execFileSync(
        process.execPath,
        [
          script,
          "run",
          dry,
          "1/1",
          join(dir, "results.jsonl"),
          "--deadline",
          "5",
          "--tests",
          "engine",
          "5",
          process.execPath,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/No tests for 1 mutant\(s\), as kill:c1 in plugin\/src\/C\.cpp/u);
  });
});

describe("reading the tests to run against", () => {
  test.each([
    [
      "a module given no limit",
      [
        "--tests",
        "engine",
        "5",
        process.execPath,
        "--tests",
        "plugin",
        "fast",
        process.execPath,
      ],
    ],
    ["a module given no binary", ["--tests", "engine", "5"]],
    ["no tests at all", []],
  ])(
    "refuses --tests with %s, rather than leave a module's mutants out",
    (_, tests) => {
      const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
      const dry = join(dir, "dry.json");
      writeFileSync(dry, JSON.stringify(dryRun));
      const script = join(import.meta.dirname, "..", "mutation-run.ts");
      const result = spawnSync(
        process.execPath,
        [
          script,
          "run",
          dry,
          "1/1",
          join(dir, "r.jsonl"),
          "--deadline",
          "5",
          ...tests,
        ],
        { encoding: "utf8" },
      );
      expect(result.status).toBe(2);
      expect(result.stderr).toContain("--tests <module> <s> <binary>");
    },
  );
});

describe("checking the tests pass with no mutant", () => {
  test("refuses to run a share whose tests fail with no mutant, as every mutant would read as noticed", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    // Holds every mutant's switch, past the line that fails.
    const binary = join(dir, "engine_tests");
    writeFileSync(
      binary,
      `#!/bin/sh\nexit 1\n# ${mutantIds(dryRun).join("\0")}\0\n`,
    );
    chmodSync(binary, 0o755);
    const results = join(dir, "results.jsonl");
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    expect(() =>
      execFileSync(
        process.execPath,
        [
          script,
          "run",
          dry,
          "1/1",
          results,
          "--deadline",
          "5",
          "--tests",
          "engine",
          "5",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/engine_tests fails, or takes more than 5 s, with no mutant/u);
    expect(existsSync(results)).toBe(false);
  });

  test("passes tests that pass", async () => {
    expect(
      await testsPass({
        limitSeconds: 5,
        command: process.execPath,
        args: [fake],
      }),
    ).toBe(true);
  });

  test("fails tests that fail with no mutant, as every mutant would read as noticed", async () => {
    expect(
      await testsPass({
        limitSeconds: 5,
        command: process.execPath,
        args: ["-e", "process.exit(1)"],
      }),
    ).toBe(false);
  });

  test("fails tests that outlast the limit with no mutant, as every mutant would time out and read as noticed", async () => {
    expect(
      await testsPass({
        limitSeconds: 1,
        command: process.execPath,
        args: ["-e", "setInterval(() => undefined, 1000)"],
      }),
    ).toBe(false);
  });

  test("fails tests that cannot start", async () => {
    expect(
      await testsPass({ limitSeconds: 5, command: "/no/such/tests", args: [] }),
    ).toBe(false);
  });
});
