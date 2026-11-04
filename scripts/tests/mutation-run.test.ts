// Running the C++ mutants a share at a time: each share gets some of every file, each mutant's
// result is kept as soon as it is known, a mutant that hangs is stopped at the limit, and every
// result is written within the budget, the mutants it leaves reported as not run rather than lost.
import {
  chmodSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { pathToFileURL } from "node:url";

import { describe, expect, test } from "vitest";

import { execFileSync } from "node:child_process";

import {
  combine,
  merge,
  missingSwitches,
  mutantIds,
  run,
  shareOf,
  baseline,
  limitOf,
  listedTests,
  reportedTimes,
  statusOf,
  wholeSeconds,
} from "../mutation-run.ts";
import type { BaselineReport } from "../mutation-run.ts";
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

const fakeGtest = join(import.meta.dirname, "fixtures", "fake-gtest.mjs");

// Two plain tests, as a module whose tests pass with no mutant.
const passing = {
  startupMs: 20,
  suites: [
    {
      name: "Suite",
      tests: [
        { name: "A", ms: 10 },
        { name: "B", ms: 10 },
      ],
    },
  ],
};

// Parameterised tests, one taking under GoogleTest's millisecond, beside a plain one.
const parameterised = {
  startupMs: 20,
  suites: [
    { name: "Matrix", tests: [{ name: "HoldsItsSize", ms: 50 }] },
    {
      name: "EveryRate/MovingMatchWork",
      tests: [
        { name: "RedoesTheSums/0", param: "384k", ms: 200 },
        { name: "RedoesTheSums/1", param: "44k1", ms: 0 },
      ],
    },
  ],
};

// A GoogleTest-like binary (fixtures/fake-gtest.mjs) from `spec`, holding every mutant's switch.
// Node runs it directly, not through a shell: Linux's sh (dash) drops environment variables whose
// names are not a shell's, as every mutant's switch is ("kill:a1").
function gtestBinary(dir: string, name: string, spec: object): string {
  const specPath = join(dir, `${name}.json`);
  writeFileSync(specPath, JSON.stringify(spec));
  const binary = join(dir, name);
  writeFileSync(
    binary,
    [
      `#!${process.execPath}`,
      `process.argv.splice(2, 0, ${JSON.stringify(specPath)});`,
      `void import(${JSON.stringify(pathToFileURL(fakeGtest).href)});`,
      `// ${mutantIds(dryRun).join("\0")}\0`,
      "",
    ].join("\n"),
  );
  chmodSync(binary, 0o755);
  return binary;
}

// When a budget of `seconds` from now ends.
const inSeconds = (seconds: number) => Date.now() + seconds * 1000;

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

describe("running a share", () => {
  test("writes each result as soon as it is known, so a share cut short keeps what it ran", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    let finished = false;
    const running = run({
      ids: ["kill:a", "hang:b"],
      results,
      limitsSeconds: [3],
      endsAt: inSeconds(60),
      tests: [{ command: process.execPath, args: [fake] }],
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
      ids: ["kill:a", "survive:b", "hang:c"],
      results,
      limitsSeconds: [1],
      endsAt: inSeconds(60),
      tests: [{ command: process.execPath, args: [fake] }],
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

  test("reports the mutants left when the budget ends as not run, not as lost", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      ids: ["hang:a", "kill:b", "kill:c"],
      results,
      limitsSeconds: [1],
      endsAt: inSeconds(0.8),
      tests: [{ command: process.execPath, args: [fake] }],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["NotRun", "NotRun", "NotRun"]);
    expect(readFileSync(results, "utf8").trim().split("\n")).toHaveLength(3);
  });

  // CI cancels a share's job soon after its budget: a mutant still running then would be lost, and
  // its hang, if it is one, never shown.
  test("stops a mutant still running when the budget ends and reports it as not run, not as timed out", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const started = Date.now();
    const done = await run({
      ids: ["hang:a"],
      results,
      limitsSeconds: [30],
      endsAt: inSeconds(1),
      tests: [{ command: process.execPath, args: [fake] }],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["NotRun"]);
    // Started, and stopped at the budget's end rather than at its limit.
    expect(Date.now() - started).toBeGreaterThan(900);
    expect(Date.now() - started).toBeLessThan(5000);
    expect(readFileSync(results, "utf8")).toContain('"NotRun"');
  });

  test("still counts a mutant started with less time left than its limit, when its tests end within it", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      ids: ["kill:a", "survive:b"],
      results,
      limitsSeconds: [600],
      endsAt: inSeconds(20),
      tests: [{ command: process.execPath, args: [fake] }],
      workers: 2,
    });
    expect(Object.fromEntries(done.map((r) => [r.id, r.status]))).toEqual({
      "kill:a": "Killed",
      "survive:b": "Survived",
    });
  });

  test("reports a mutant whose limit fits in the time left as timed out at its own limit", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      ids: ["hang:a"],
      results,
      limitsSeconds: [1],
      endsAt: inSeconds(30),
      tests: [{ command: process.execPath, args: [fake] }],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["Timeout"]);
  });

  test("ends the run with no mutant within the budget, and runs no mutant after it", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    // Hangs with no mutant, as a slow runner's tests outlast the budget; holds every switch.
    const binary = join(dir, "engine_tests");
    writeFileSync(
      binary,
      `#!/bin/sh\nexec sleep 60\n# ${mutantIds(dryRun).join("\0")}\0\n`,
    );
    chmodSync(binary, 0o755);
    const results = join(dir, "results.jsonl");
    const started = Date.now();
    expect(() =>
      execFileSync(
        process.execPath,
        [
          join(import.meta.dirname, "..", "mutation-run.ts"),
          "run",
          dry,
          "1/1",
          results,
          "--budget",
          "1",
          "--",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/the budget's 1 s/u);
    expect(Date.now() - started).toBeLessThan(10000);
    expect(existsSync(results)).toBe(false);
  });

  test("counts a mutant as noticed when any of several test binaries fails on it, as the Audio Unit's view tests notice some its other tests miss", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      ids: ["later:a", "survive:b", "kill:c", "hang:d"],
      results,
      limitsSeconds: [1, 1],
      endsAt: inSeconds(60),
      tests: [
        { command: process.execPath, args: [fake] },
        { command: process.execPath, args: [fake, "later"] },
      ],
      workers: 4,
    });
    expect(Object.fromEntries(done.map((r) => [r.id, r.status]))).toEqual({
      "later:a": "Killed",
      "survive:b": "Survived",
      "kill:c": "Killed",
      "hang:d": "Timeout",
    });
  });

  test("counts a mutant only the first of several test binaries fails on as noticed, whatever the later ones do", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const done = await run({
      ids: ["first:a", "survive:b"],
      results,
      limitsSeconds: [5, 5],
      endsAt: inSeconds(60),
      tests: [
        { command: process.execPath, args: [fake, "first"] },
        { command: process.execPath, args: [fake] },
      ],
      workers: 2,
    });
    expect(Object.fromEntries(done.map((r) => [r.id, r.status]))).toEqual({
      "first:a": "Killed",
      "survive:b": "Survived",
    });
  });

  test("runs no later test binary once one has noticed a mutant, so a later one that hangs costs nothing", async () => {
    const results = join(
      mkdtempSync(join(tmpdir(), "mutation-run-")),
      "results.jsonl",
    );
    const started = Date.now();
    const done = await run({
      ids: ["kill:a"],
      results,
      limitsSeconds: [10, 10],
      endsAt: inSeconds(60),
      tests: [
        { command: process.execPath, args: [fake] },
        {
          command: process.execPath,
          args: ["-e", "setInterval(() => undefined, 1000)"],
        },
      ],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["Killed"]);
    expect(Date.now() - started).toBeLessThan(5000);
  });

  test("runs one mutant at a time with --workers 1, for tests that cannot run beside each other", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    // Fails a run while another holds its lock, as the Audio Unit's view tests share the window
    // server: a survivor would then read as killed.
    const binary = gtestBinary(dir, "au_view_tests", {
      ...passing,
      lock: join(dir, "lock"),
    });
    const results = join(dir, "results.jsonl");
    execFileSync(
      process.execPath,
      [
        join(import.meta.dirname, "..", "mutation-run.ts"),
        "run",
        dry,
        "1/1",
        results,
        "--budget",
        "60",
        "--workers",
        "1",
        "--",
        binary,
      ],
      { stdio: "pipe" },
    );
    const statuses = readFileSync(results, "utf8")
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line) as { id: string; status: string })
      .map(({ id, status }) => [id, status]);
    expect(Object.fromEntries(statuses)).toEqual({
      "kill:a1": "Killed",
      "kill:a3": "Killed",
      "survive:a2": "Survived",
      "survive:b1": "Survived",
    });
  });
});

describe("listing several test binaries' mutants together", () => {
  test("lists every module's mutants, so that a share of the list holds some of each", () => {
    const plugin: Report = {
      files: { "plugin/src/C.cpp": { mutants: [mutant("kill:c1")] } },
    };
    expect(mutantIds(combine([dryRun, plugin]))).toEqual(
      [...mutantIds(dryRun), "kill:c1"].sort(),
    );
  });

  test("lists no mutant from no dry run", () => {
    expect(mutantIds(combine([]))).toEqual([]);
  });

  test("prints the lists as one from the command line (scripts/mutation-cpp.sh)", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const engine = join(dir, "dry-engine.json");
    const plugin = join(dir, "dry-plugin.json");
    writeFileSync(engine, JSON.stringify(dryRun));
    writeFileSync(
      plugin,
      JSON.stringify({
        files: { "plugin/src/C.cpp": { mutants: [mutant("kill:c1")] } },
      }),
    );
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    const printed = JSON.parse(
      execFileSync(process.execPath, [script, "combine", engine, plugin], {
        encoding: "utf8",
      }),
    ) as Report;
    expect(Object.keys(printed.files)).toEqual([
      "engine/src/A.cpp",
      "engine/src/B.cpp",
      "plugin/src/C.cpp",
    ]);
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

describe("the run's command line", () => {
  test.each([
    ["no number of workers", ["--workers", "0"], []],
    ["workers that are not a number", ["--workers", "abc"], []],
    ["a -- with no test binary after it", [], ["--"]],
  ])(
    "is refused with %s, rather than running no mutant or an empty command",
    (_, options, after) => {
      const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
      const dry = join(dir, "dry.json");
      writeFileSync(dry, JSON.stringify(dryRun));
      const binary = join(dir, "au_tests");
      writeFileSync(
        binary,
        `#!/bin/sh\nexit 0\n# ${mutantIds(dryRun).join("\0")}\0\n`,
      );
      chmodSync(binary, 0o755);
      const results = join(dir, "results.jsonl");
      let status: number | null = 0;
      try {
        execFileSync(
          process.execPath,
          [
            join(import.meta.dirname, "..", "mutation-run.ts"),
            "run",
            dry,
            "1/1",
            results,
            "--budget",
            "5",
            ...options,
            "--",
            binary,
            ...after,
          ],
          { stdio: "pipe" },
        );
      } catch (error) {
        status = (error as { status: number | null }).status;
      }
      expect(status).toBe(2);
      expect(existsSync(results)).toBe(false);
    },
  );
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
          "--budget",
          "1",
          "--",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/no switch for 4 of the 4 mutants/u);
  });
});

describe("the tests' run with no mutant", () => {
  test("refuses to run a share whose tests fail with no mutant, as every mutant would read as noticed", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    const binary = gtestBinary(dir, "engine_tests", {
      ...passing,
      failWithNoMutant: true,
    });
    const results = join(dir, "results.jsonl");
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    expect(() =>
      execFileSync(
        process.execPath,
        [script, "run", dry, "1/1", results, "--budget", "5", "--", binary],
        { stdio: "pipe" },
      ),
    ).toThrow(/fails with no mutant/u);
    expect(existsSync(results)).toBe(false);
  });

  test("runs no test for a share with no mutant, as a module with fewer mutants than shares leaves", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(
      dry,
      JSON.stringify({
        files: { "plugin/src/C.cpp": { mutants: [mutant("kill:c1")] } },
      }),
    );
    // Fails with no mutant: run, it would refuse the share. Share 2 of 2 holds no mutant.
    const binary = join(dir, "plugin_tests");
    writeFileSync(binary, "#!/bin/sh\nexit 1\n");
    chmodSync(binary, 0o755);
    const results = join(dir, "results.jsonl");
    const script = join(import.meta.dirname, "..", "mutation-run.ts");
    const printed = execFileSync(
      process.execPath,
      [script, "run", dry, "2/2", results, "--budget", "5", "--", binary],
      { encoding: "utf8" },
    );
    expect(printed).toContain("0 mutants run");
    expect(readFileSync(results, "utf8")).toBe("");
  });

  test("times each test under the name the binary lists, parameterised ones and those under a millisecond included, so each mutant's limit follows its tests", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "engine_tests", parameterised);
    const result = await baseline({
      endsAt: inSeconds(60),
      tests: [{ command: binary, args: [] }],
      workers: 3,
    });
    if (typeof result === "string") {
      throw new Error(result);
    }
    const [times] = result;
    expect(Object.fromEntries(times?.testSeconds ?? [])).toEqual({
      "EveryRate/MovingMatchWork.RedoesTheSums/0": 0.2,
      "EveryRate/MovingMatchWork.RedoesTheSums/1": 0,
      "Matrix.HoldsItsSize": 0.05,
    });
    expect(times?.startupSeconds).toBeGreaterThan(0);
    expect(
      wholeSeconds(times ?? { startupSeconds: 0, testSeconds: new Map() }),
    ).toBeCloseTo((times?.startupSeconds ?? 0) + 0.25, 6);
  });

  test("fails when a test the binary lists has no time in its reports, rather than guess every limit from a broken join", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "engine_tests", {
      ...parameterised,
      omitFromJson: ["EveryRate/MovingMatchWork.RedoesTheSums/1"],
    });
    expect(
      await baseline({
        endsAt: inSeconds(60),
        tests: [{ command: binary, args: [] }],
        workers: 2,
      }),
    ).toMatch(/EveryRate\/MovingMatchWork\.RedoesTheSums\/1/u);
  });

  test("spreads the tests over the workers as GoogleTest shards, so the run with no mutant takes a fraction of the suite's time", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const slow = {
      startupMs: 0,
      suites: [
        {
          name: "Work",
          tests: ["A", "B", "C", "D"].map((name) => ({ name, ms: 600 })),
        },
      ],
    };
    const binary = gtestBinary(dir, "engine_tests", slow);
    const started = Date.now();
    const result = await baseline({
      endsAt: inSeconds(60),
      tests: [{ command: binary, args: [] }],
      workers: 4,
    });
    expect(Date.now() - started).toBeLessThan(1800);
    expect(
      typeof result === "string" ? result : result[0]?.testSeconds.size,
    ).toBe(4);
  });

  test("counts a shard that holds no test, as a module with fewer tests than workers leaves", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "plugin_tests", passing);
    const result = await baseline({
      endsAt: inSeconds(60),
      tests: [{ command: binary, args: [] }],
      workers: 5,
    });
    expect(
      typeof result === "string" ? result : result[0]?.testSeconds.size,
    ).toBe(2);
  });

  test("runs one shard with --workers 1, for tests that cannot run beside each other", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "au_view_tests", {
      ...passing,
      lock: join(dir, "lock"),
    });
    const result = await baseline({
      endsAt: inSeconds(60),
      tests: [{ command: binary, args: [] }],
      workers: 1,
    });
    expect(typeof result).not.toBe("string");
  });

  test("fails tests that fail with no mutant, as every mutant would read as noticed", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "engine_tests", {
      ...passing,
      failWithNoMutant: true,
    });
    expect(
      await baseline({
        endsAt: inSeconds(60),
        tests: [{ command: binary, args: [] }],
        workers: 2,
      }),
    ).toMatch(/fails with no mutant/u);
  });

  test("fails tests that outlast the budget with no mutant, as no mutant could then run", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const binary = gtestBinary(dir, "engine_tests", {
      startupMs: 0,
      suites: [{ name: "Slow", tests: [{ name: "A", ms: 5000 }] }],
    });
    expect(
      await baseline({
        endsAt: inSeconds(1),
        tests: [{ command: binary, args: [] }],
        workers: 1,
      }),
    ).toMatch(/outlasts the budget/u);
  });

  test("fails when any of several test binaries fails with no mutant, as every mutant would read as noticed", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const good = gtestBinary(dir, "au_tests", passing);
    const bad = gtestBinary(dir, "au_view_tests", {
      ...passing,
      failWithNoMutant: true,
    });
    expect(
      await baseline({
        endsAt: inSeconds(60),
        tests: [
          { command: good, args: [] },
          { command: bad, args: [] },
        ],
        workers: 1,
      }),
    ).toMatch(/au_view_tests fails with no mutant/u);
  });

  test("refuses to run a share whose later test binary fails with no mutant, so each binary after -- is run", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    const binary = gtestBinary(dir, "au_tests", passing);
    const failing = gtestBinary(dir, "au_view_tests", {
      ...passing,
      failWithNoMutant: true,
    });
    expect(() =>
      execFileSync(
        process.execPath,
        [
          join(import.meta.dirname, "..", "mutation-run.ts"),
          "run",
          dry,
          "1/1",
          join(dir, "results.jsonl"),
          "--budget",
          "5",
          "--",
          binary,
          "--gtest_fail_fast",
          "--",
          failing,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/au_view_tests fails with no mutant/u);
  });

  test("fails tests that cannot start", async () => {
    expect(
      await baseline({
        endsAt: inSeconds(60),
        tests: [{ command: "/no/such/tests", args: [] }],
        workers: 1,
      }),
    ).toMatch(/cannot list its tests/u);
  });
});

describe("each mutant's limit", () => {
  // A binary whose tests take `ms` with no mutant, and `slowdown` times as long with one.
  const timed = (dir: string, name: string, ms: number, slowdown = 1) =>
    gtestBinary(dir, name, {
      startupMs: 0,
      suites: [{ name: "Work", tests: [{ name: "A", ms }] }],
      mutantSlowdown: slowdown,
    });

  // Before, every engine mutant was stopped at 1,500 s while its tests took 1,490 to 1,812 s with
  // no mutant: one no test noticed read as timed out, which counts as noticed (#287).
  test("reports a mutant no test notices as surviving, however long the tests take, rather than as timed out", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    // Longer than the limit's constant, so a limit that ignored the tests' time would stop them.
    const binary = timed(dir, "engine_tests", 7000);
    const results = join(dir, "results.jsonl");
    execFileSync(
      process.execPath,
      [
        join(import.meta.dirname, "..", "mutation-run.ts"),
        "run",
        dry,
        "1/1",
        results,
        "--budget",
        "120",
        "--workers",
        "4",
        "--",
        binary,
      ],
      { stdio: "pipe" },
    );
    const statuses = readFileSync(results, "utf8")
      .trim()
      .split("\n")
      .map((line) => JSON.parse(line) as { id: string; status: string });
    expect(
      statuses
        .filter(({ id }) => id.startsWith("survive:"))
        .map((r) => r.status),
    ).toEqual(["Survived", "Survived"]);
  }, 30_000);

  test("lets a mutant that slows its tests within the limit finish, and stops one just past it", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    // Well inside and well past 1.5 times, so the processes' own start-up, which the fixture does
    // not slow, cannot move either across.
    const slower = timed(dir, "slower", 2000, 1.2);
    const slowest = timed(dir, "slowest", 2000, 2);
    const statusWith = async (binary: string) => {
      const times = await baseline({
        endsAt: inSeconds(60),
        tests: [{ command: binary, args: [] }],
        workers: 1,
      });
      if (typeof times === "string") {
        throw new Error(times);
      }
      const [only] = times;
      const done = await run({
        ids: ["survive:a"],
        results: join(dir, `${basename(binary)}.jsonl`),
        limitsSeconds: [
          limitOf(
            only ?? { startupSeconds: 0, testSeconds: new Map() },
            1.5,
            0,
          ),
        ],
        endsAt: inSeconds(60),
        tests: [{ command: binary, args: [] }],
        workers: 1,
      });
      return done[0]?.status;
    };
    expect(await statusWith(slower)).toBe("Survived");
    expect(await statusWith(slowest)).toBe("Timeout");
  }, 30_000);

  test("grows with the time of the tests the mutant runs", () => {
    const short = { startupSeconds: 1, testSeconds: new Map([["A.a", 2]]) };
    const long = {
      startupSeconds: 1,
      testSeconds: new Map([
        ["A.a", 2],
        ["A.b", 10],
      ]),
    };
    expect(limitOf(short, 2, 3)).toBe(2 * 3 + 3);
    expect(limitOf(long, 2, 3)).toBe(2 * 13 + 3);
  });

  test("gives each of several test binaries its own limit, from its own tests' time", () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const dry = join(dir, "dry.json");
    writeFileSync(dry, JSON.stringify(dryRun));
    const quick = timed(dir, "au_tests", 100);
    const slow = timed(dir, "au_view_tests", 1500);
    const out = join(dir, "baseline.json");
    execFileSync(
      process.execPath,
      [
        join(import.meta.dirname, "..", "mutation-run.ts"),
        "run",
        dry,
        "1/1",
        join(dir, "results.jsonl"),
        "--budget",
        "120",
        "--workers",
        "1",
        "--baseline-out",
        out,
        "--",
        quick,
        "--",
        slow,
      ],
      { stdio: "pipe" },
    );
    const report = JSON.parse(readFileSync(out, "utf8")) as BaselineReport;
    const [first, second] = report.binaries;
    expect([first?.name, second?.name]).toEqual(["au_tests", "au_view_tests"]);
    for (const binary of report.binaries) {
      expect(binary.limitSeconds).toBeCloseTo(
        report.factor * binary.seconds + report.constantSeconds,
        6,
      );
    }
    expect((second?.seconds ?? 0) - (first?.seconds ?? 0)).toBeGreaterThan(1);
  }, 30_000);

  test("reports a mutant as not run when the budget ends in its second test binary", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const done = await run({
      ids: ["survive:a"],
      results: join(dir, "results.jsonl"),
      limitsSeconds: [10, 10],
      endsAt: inSeconds(1.5),
      tests: [
        { command: process.execPath, args: [fake] },
        {
          command: process.execPath,
          args: ["-e", "setInterval(() => undefined, 1000)"],
        },
      ],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["NotRun"]);
  });

  test("caps a mutant's binaries together, not each on its own, so their limits' sum cannot outlast the budget", async () => {
    const dir = mkdtempSync(join(tmpdir(), "mutation-run-"));
    const second = timed(dir, "au_view_tests", 60000);
    const started = Date.now();
    const done = await run({
      ids: ["survive:a"],
      results: join(dir, "results.jsonl"),
      // Each fits in the budget; together they do not.
      limitsSeconds: [2, 2.5],
      endsAt: inSeconds(3),
      tests: [
        { command: timed(dir, "au_tests", 1000), args: [] },
        { command: second, args: [] },
      ],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["NotRun"]);
    expect(Date.now() - started).toBeLessThan(4500);
  });
});

describe("reading GoogleTest's list and reports", () => {
  test("lists each test as Suite.Name, without gtest_main's first line or a parameter's comment", () => {
    expect(
      listedTests(
        [
          "Running main() from /src/googletest/src/gtest_main.cc",
          "Matrix.",
          "  HoldsItsSize",
          "EveryRate/MovingMatchWork.",
          "  RedoesTheSums/0  # GetParam() = 384k",
          "",
        ].join("\n"),
      ),
    ).toEqual([
      "Matrix.HoldsItsSize",
      "EveryRate/MovingMatchWork.RedoesTheSums/0",
    ]);
  });

  test("reads each test's time under its suite's name, as GoogleTest's JSON nests them", () => {
    const json = JSON.stringify({
      testsuites: [
        {
          name: "EveryRate/MovingMatchWork",
          testsuite: [
            { name: "RedoesTheSums/0", time: "0.016s", classname: "x" },
            { name: "RedoesTheSums/1", time: "0s", classname: "x" },
          ],
        },
      ],
    });
    expect(Object.fromEntries(reportedTimes(json))).toEqual({
      "EveryRate/MovingMatchWork.RedoesTheSums/0": 0.016,
      "EveryRate/MovingMatchWork.RedoesTheSums/1": 0,
    });
  });
});
