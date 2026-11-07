// scripts/check-linux.sh, the shared code's sanitizers and lint: each check runs whatever another
// found, so one run shows every finding, and the lint comes first, without a full build.
import { realpathSync } from "node:fs";
import { afterEach, describe, expect, test } from "vitest";

import { Layout, summary, variables } from "./check-harness.ts";

const tools = ["uname", "nproc", "cmake", "ctest", "git", "docker"];
let layout: Layout | undefined;
afterEach(() => {
  layout?.remove();
  layout = undefined;
});

const run = (args: string[] = [], env: Record<string, string> = {}) => {
  layout ??= new Layout(["scripts/check-linux.sh", "scripts/stages.sh"], tools);
  return layout.run(["scripts/check-linux.sh", ...args], env);
};

// Each check, the call that fails it, and the stages it reports.
const checks = [
  { check: "lint", fails: "^clang-tidy", stages: ["Linux lint"] },
  {
    check: "asan",
    fails: "^cmake --build --preset linux -j",
    stages: ["Linux asan build", "Linux asan tests"],
  },
  {
    check: "tsan",
    fails: "^cmake --build --preset linux-tsan -j",
    stages: ["Linux tsan build", "Linux tsan tests"],
  },
  {
    check: "rtsan",
    fails: "^cmake --build --preset linux-rtsan -j",
    stages: ["Linux rtsan build", "Linux rtsan tests"],
  },
];

describe("a failing check", () => {
  test.each(checks)(
    "($check) does not stop the others",
    ({ check, fails, stages }) => {
      const { status, output } = run([], variables(["STUB_FAIL", fails]));
      const reported = summary(output);
      expect(reported.get(stages[0] ?? "")).toMatch(/^FAILED/);
      for (const other of checks.filter((each) => each.check !== check)) {
        for (const name of other.stages) {
          expect(reported.get(name), name).toBe("passed");
        }
      }
      expect(status).toBe(1);
    },
  );

  test.each(checks.filter(({ stages }) => stages.length > 1))(
    "($check) build skips its own tests",
    ({ fails, stages }) => {
      const { calls, output } = run([], variables(["STUB_FAIL", fails]));
      expect(summary(output).get(stages[1] ?? "")).toMatch(/^skipped/);
      const preset = /--preset (\S+)/.exec(fails)?.[1] ?? "";
      expect(calls).not.toContainEqual(
        expect.stringMatching(`^ctest --preset ${preset} `),
      );
    },
  );
});

test("the lint runs first, before any full build", () => {
  const { calls, status } = run();
  const lint = calls.findIndex((call) => call.startsWith("clang-tidy"));
  const fullBuild = calls.findIndex((call) =>
    /^cmake --build --preset \S+ -j/.test(call),
  );
  expect(lint).toBeGreaterThanOrEqual(0);
  expect(fullBuild).toBeGreaterThan(lint);
  expect(status).toBe(0);
});

test("an unknown check runs nothing", () => {
  const { calls, status } = run(["asan", "bogus"]);
  expect(
    calls.filter((call) => /^(cmake|ctest|clang-tidy)/.test(call)),
  ).toEqual([]);
  expect(status).not.toBe(0);
});

describe("CTEST_SHARD", () => {
  test("runs one share of the AddressSanitizer tests", () => {
    const { calls } = run(["asan", "tsan"], variables(["CTEST_SHARD", "2/4"]));
    expect(calls).toContainEqual(
      expect.stringMatching(/^ctest --preset linux .*-I 2,,4/),
    );
    expect(calls).toContainEqual(
      expect.stringMatching(/^ctest --preset linux-tsan (?!.*-I)/),
    );
  });

  test("unset runs every test", () => {
    const { calls } = run(["asan"]);
    expect(calls).toContainEqual(
      expect.stringMatching(/^ctest --preset linux (?!.*-I)/),
    );
  });

  test("not of the form k/n stops the run", () => {
    const { calls, status } = run(["asan"], variables(["CTEST_SHARD", "2"]));
    expect(calls.filter((call) => /^(cmake|ctest)/.test(call))).toEqual([]);
    expect(status).not.toBe(0);
  });
});

test("BUILD_ONLY=1 runs the builds and nothing else", () => {
  const { calls, status } = run([], variables(["BUILD_ONLY", "1"]));
  for (const preset of ["linux", "linux-tsan", "linux-rtsan"]) {
    expect(calls).toContainEqual(
      expect.stringMatching(new RegExp(`^cmake --build --preset ${preset} -j`)),
    );
  }
  expect(calls.filter((call) => /^(ctest|clang-tidy)/.test(call))).toEqual([]);
  expect(status).toBe(0);
});

// On macOS the checks run in Docker, which sees only the checkout. A worktree's git folder lives
// in the main checkout's, outside it, and without it git ls-files fails there, so the lint did.
describe("in Docker", () => {
  const dockerRun = (common: string) =>
    run(
      ["lint"],
      variables(["STUB_UNAME", "Darwin"], ["STUB_GIT_COMMON", common]),
    ).calls.find((call) => call.startsWith("docker run")) ?? "";

  test("a worktree's git folder is mounted, read-only, where git looks for it", () => {
    expect(dockerRun("/elsewhere/repo/.git")).toContain(
      "-v /elsewhere/repo/.git:/elsewhere/repo/.git:ro",
    );
  });

  test("a checkout's own git folder is not mounted again", () => {
    layout ??= new Layout(
      ["scripts/check-linux.sh", "scripts/stages.sh"],
      tools,
    );
    expect(dockerRun(`${realpathSync(layout.root)}/.git`)).not.toContain(":ro");
  });
});
