// Running the C++ mutants a share at a time: each share gets some of every file, each mutant's
// result is kept as soon as it is known, a mutant that hangs is stopped at the limit, and the
// mutants left at the deadline are reported as not run rather than lost.
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

import { execFileSync } from "node:child_process";

import {
  merge,
  missingSwitches,
  mutantIds,
  run,
  shareOf,
  statusOf,
  testsPass,
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
      limitSeconds: 3,
      deadlineSeconds: 60,
      command: process.execPath,
      args: [fake],
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
      limitSeconds: 1,
      deadlineSeconds: 60,
      command: process.execPath,
      args: [fake],
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
      ids: ["hang:a", "kill:b", "kill:c"],
      results,
      limitSeconds: 1,
      deadlineSeconds: 0.5,
      command: process.execPath,
      args: [fake],
      workers: 1,
    });
    expect(done.map((r) => r.status)).toEqual(["Timeout", "NotRun", "NotRun"]);
    expect(readFileSync(results, "utf8").trim().split("\n")).toHaveLength(3);
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
          "--limit",
          "1",
          "--deadline",
          "1",
          "--",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/no switch for 4 of the 4 mutants/u);
  });
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
          "--limit",
          "5",
          "--deadline",
          "5",
          "--",
          binary,
        ],
        { stdio: "pipe" },
      ),
    ).toThrow(/The tests fail, or take more than 5 s, with no mutant/u);
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
