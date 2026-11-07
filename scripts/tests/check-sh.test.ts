// scripts/check.sh: every stage runs whatever another finds, the cheap checks come first and the
// performance limits last, and a summary lists the failures, so one run shows every finding.
import { afterEach, describe, expect, test } from "vitest";

import { Layout, summary, variables } from "./check-harness.ts";

const tools = [
  "cmake",
  "ctest",
  "npm",
  "npx",
  "git",
  "sysctl",
  "xcrun",
  "brew",
];
const pluginval = "build/tools/pluginval.app/Contents/MacOS/pluginval";
let layout: Layout | undefined;
afterEach(() => {
  layout?.remove();
  layout = undefined;
});

const run = (env: Record<string, string> = {}, packages = true) => {
  if (layout === undefined) {
    layout = new Layout(["scripts/check.sh", "scripts/stages.sh"], tools, [
      "scripts/performance.sh",
      "scripts/validate-au.sh",
      "scripts/check-linux.sh",
      pluginval,
    ]);
    if (packages) {
      layout.stub("node_modules/.keep");
    }
  }
  return layout.run(["scripts/check.sh"], env);
};

// Each stage, the call that fails it, and the stages its failure skips.
const stages = [
  { stage: "C++ format", fails: "^clang-format", skips: [] },
  { stage: "TypeScript typecheck", fails: "^npm run typecheck", skips: [] },
  { stage: "TypeScript lint", fails: "^npm run lint", skips: [] },
  { stage: "TypeScript format", fails: "^npm run format:check", skips: [] },
  { stage: "macOS lint", fails: "^clang-tidy", skips: [] },
  { stage: "Linux lint", fails: "^check-linux.sh lint$", skips: [] },
  {
    stage: "C++ build",
    fails: "^cmake --build --preset dev -j",
    skips: ["C++ tests", "pluginval", "Audio Unit validation"],
  },
  { stage: "C++ tests", fails: "^ctest --preset dev", skips: [] },
  { stage: "pluginval", fails: "^pluginval ", skips: [] },
  { stage: "Audio Unit validation", fails: "^validate-au.sh", skips: [] },
  { stage: "TypeScript tests", fails: "^npm test", skips: [] },
  { stage: "App tests", fails: "^npm run test:app", skips: [] },
  { stage: "Page build", fails: "^npm run build", skips: [] },
  {
    stage: "asan build",
    fails: "^cmake --build --preset asan -j",
    skips: ["asan tests"],
  },
  { stage: "asan tests", fails: "^ctest --preset asan", skips: [] },
  {
    stage: "tsan build",
    fails: "^cmake --build --preset tsan -j",
    skips: ["tsan tests"],
  },
  { stage: "tsan tests", fails: "^ctest --preset tsan", skips: [] },
  {
    stage: "rtsan build",
    fails: "^cmake --build --preset rtsan -j",
    skips: ["rtsan tests"],
  },
  { stage: "rtsan tests", fails: "^ctest --preset rtsan", skips: [] },
  {
    stage: "Linux sanitizers",
    fails: "^check-linux.sh asan tsan rtsan",
    skips: [],
  },
  { stage: "Performance limits", fails: "^performance.sh", skips: [] },
];

test("a clean run passes every stage, each failable above", () => {
  const { status, output } = run();
  const reported = summary(output);
  expect([...reported.keys()]).toEqual(stages.map(({ stage }) => stage));
  expect([...reported.values()].every((result) => result === "passed")).toBe(
    true,
  );
  expect(status).toBe(0);
});

test.each(stages)(
  "a failing $stage leaves the stages that do not need it to run",
  ({ stage, fails, skips }) => {
    const { status, output } = run(variables(["STUB_FAIL", fails]));
    const reported = summary(output);
    expect(reported.get(stage)).toMatch(/^FAILED/);
    for (const [name, result] of reported) {
      if (skips.includes(name)) {
        expect(result, name).toBe(`skipped (needs ${stage})`);
      } else if (name !== stage) {
        expect(result, name).toBe("passed");
      }
    }
    expect(output).toContain(`1 of ${String(stages.length)} stages failed.`);
    expect(status).toBe(1);
  },
);

test("the static checks run before any full build, and performance after everything", () => {
  const { calls } = run();
  const fullBuild = calls.findIndex((call) =>
    /^cmake --build --preset \S+ -j/.test(call),
  );
  for (const check of [
    /^clang-format/,
    /^npm run typecheck/,
    /^npm run lint/,
    /^npm run format:check/,
    /^clang-tidy/,
    /^check-linux.sh lint$/,
  ]) {
    const index = calls.findIndex((call) => check.test(call));
    expect(index, check.source).toBeGreaterThanOrEqual(0);
    expect(index, check.source).toBeLessThan(fullBuild);
  }
  expect(calls.at(-1)).toMatch(/^performance.sh/);
  // What the macOS lint reads: the configured commands and the generated protocol header.
  const lint = calls.findIndex((call) => call.startsWith("clang-tidy"));
  const header = calls.indexOf(
    "cmake --build --preset asan --target protocol_header",
  );
  expect(header).toBeGreaterThanOrEqual(0);
  expect(header).toBeLessThan(lint);
  expect(calls.indexOf("cmake --preset asan")).toBeLessThan(header);
});

test("SKIP_APP_TESTS=1 alone leaves out only the app tests", () => {
  const { calls, output } = run(variables(["SKIP_APP_TESTS", "1"]));
  const reported = [...summary(output).keys()];
  expect(reported).not.toContain("App tests");
  expect(reported).toContain("TypeScript tests");
  expect(calls.filter((call) => call.startsWith("npm run test:app"))).toEqual(
    [],
  );
});

test("a performance miss runs once and says how to rerun it alone", () => {
  const { calls, output } = run(variables(["STUB_FAIL", "^performance.sh"]));
  expect(
    calls.filter((call) => call.startsWith("performance.sh")),
  ).toHaveLength(1);
  expect(output).toContain("Performance limits: scripts/performance.sh");
});

test("a missing LLVM tool stops the run before any stage", () => {
  const { calls, status } = run(variables(["LLVM_BIN", "/nonexistent"]));
  expect(calls.filter((call) => /^(cmake|ctest|npm|clang)/.test(call))).toEqual(
    [],
  );
  expect(status).not.toBe(0);
});

test("installs the packages first when they are missing", () => {
  const { calls, status } = run({}, false);
  expect(calls.findIndex((call) => call === "npm ci")).toBe(
    calls.findIndex((call) => call.startsWith("npm")),
  );
  expect(calls).toContain("npm ci");
  expect(status).toBe(0);
});

describe("BUILD_ONLY=1", () => {
  test("with CHECKS=linux runs the Linux builds, not the lint", () => {
    const { calls, status } = run(
      variables(["BUILD_ONLY", "1"], ["CHECKS", "linux"]),
    );
    expect(calls).toContain("check-linux.sh asan tsan rtsan");
    expect(calls).not.toContain("check-linux.sh lint");
    expect(status).toBe(0);
  });

  test.each(["main", "glue"])(
    "with CHECKS=%s runs the builds and nothing else",
    (part) => {
      const { calls, status } = run(
        variables(["BUILD_ONLY", "1"], ["CHECKS", part]),
      );
      expect(calls).toContainEqual(
        expect.stringMatching(/^cmake --build --preset \S+ -j/),
      );
      expect(
        calls.filter((call) =>
          /^(ctest|clang-tidy|clang-format|pluginval|validate-au|performance|npm run|npm test)/.test(
            call,
          ),
        ),
      ).toEqual([]);
      expect(status).toBe(0);
    },
  );
});

// A guard, not a change: what CI's macOS jobs check stays the same.
describe("CI's settings", () => {
  const skips = variables(
    ["SKIP_PERFORMANCE", "1"],
    ["SKIP_FORMAT", "1"],
    ["SKIP_APP_TESTS", "1"],
    ["SKIP_TYPESCRIPT", "1"],
  );
  test.each([
    {
      job: "macos",
      checks: "main",
      expected: [
        "C++ build",
        "C++ tests",
        "pluginval",
        "Audio Unit validation",
      ],
    },
    {
      job: "macos-glue",
      checks: "glue",
      expected: [
        "macOS lint",
        "asan build",
        "asan tests",
        "tsan build",
        "tsan tests",
        "rtsan build",
        "rtsan tests",
      ],
    },
  ])(
    "for the $job job run the stages it ran before",
    ({ checks, expected }) => {
      const { output, status } = run({
        ...skips,
        ...variables(["CHECKS", checks]),
      });
      expect([...summary(output).keys()]).toEqual(expected);
      expect(status).toBe(0);
    },
  );
});
