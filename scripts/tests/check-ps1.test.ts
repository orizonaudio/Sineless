// scripts\check.ps1, the Windows check: every stage runs whatever another finds, the cheap checks
// first and the performance limit last. Run for real under PowerShell on Linux with stubbed tools
// (CI's Linux runners have PowerShell); skipped where PowerShell is missing.
import { spawnSync } from "node:child_process";
import { afterEach, describe, expect, test } from "vitest";

import { Layout, summary, variables } from "./check-harness.ts";

const pwsh =
  spawnSync("pwsh", ["-NoProfile", "-Command", "exit 0"]).status === 0;
const tools = [
  "cmake",
  "ctest",
  "git",
  "npm.cmd",
  "npx.cmd",
  "robocopy",
  "dumpbin",
];
let layout: Layout | undefined;
afterEach(() => {
  layout?.remove();
  layout = undefined;
});

const run = (env: Record<string, string> = {}) => {
  if (layout === undefined) {
    layout = new Layout(
      ["scripts/check.ps1", "scripts/stages.ps1", "scripts/windows-env.ps1"],
      tools,
      ["scripts/performance.ps1", "build/tools/pluginval.exe"],
    );
    layout.stub(".stubs/llvm/clang-format.exe");
    layout.stub("node_modules/.keep");
  }
  return layout.run(
    ["-NoProfile", "-File", "scripts/check.ps1"],
    variables(["PROCESSOR_ARCHITECTURE", "AMD64"], ...Object.entries(env)),
    "pwsh",
  );
};

// Each stage, the call that fails it, and the stages its failure skips.
const stages = [
  { stage: "C++ format", fails: "^clang-format", skips: [] },
  { stage: "TypeScript typecheck", fails: "^npm.cmd run typecheck", skips: [] },
  { stage: "TypeScript lint", fails: "^npm.cmd run lint", skips: [] },
  { stage: "TypeScript format", fails: "^npm.cmd run format:check", skips: [] },
  {
    stage: "C++ build: windows-x64",
    fails: "^cmake --build --preset windows-x64 ",
    skips: ["C++ tests: windows-x64", "Bundle", "pluginval"],
  },
  {
    stage: "C++ tests: windows-x64",
    fails: "^ctest --preset windows-x64 ",
    skips: [],
  },
  // robocopy fails from exit code 8.
  { stage: "Bundle", fails: "^robocopy", status: "8", skips: ["pluginval"] },
  { stage: "pluginval", fails: "^pluginval.exe", skips: [] },
  { stage: "TypeScript tests", fails: "^npm.cmd test", skips: [] },
  { stage: "App tests", fails: "^npm.cmd run test:app", skips: [] },
  { stage: "Page build", fails: "^npm.cmd run build", skips: [] },
  {
    stage: "Glue build",
    fails: "^cmake --build --preset windows-x64-glue",
    skips: ["Glue tests"],
  },
  { stage: "Glue tests", fails: "^ctest --preset windows-x64-glue", skips: [] },
  { stage: "Performance limits", fails: "^performance.ps1", skips: [] },
];

describe.skipIf(!pwsh)("check.ps1", () => {
  test("passes every stage on a clean run, each failable above", () => {
    const { status, output } = run();
    expect([...summary(output).keys()]).toEqual(
      stages.map(({ stage }) => stage),
    );
    expect(status).toBe(0);
  });

  test.each(stages)(
    "a failing $stage leaves the stages that do not need it to run",
    ({ stage, fails, status: failure = "1", skips }) => {
      const { status, output } = run(
        variables(["STUB_FAIL", fails], ["STUB_STATUS", failure]),
      );
      const reported = summary(output);
      expect(reported.get(stage)).toMatch(/^FAILED/);
      for (const [name, result] of reported) {
        if (skips.includes(name)) {
          expect(result, name).toMatch(/^skipped \(needs /);
        } else if (name !== stage) {
          expect(result, name).toBe("passed");
        }
      }
      expect(status).toBe(1);
    },
  );

  test("on an ARM64 machine runs no glue stage", () => {
    const { output, status } = run(
      variables(["PROCESSOR_ARCHITECTURE", "ARM64"], ["CHECKS", "glue"]),
    );
    expect([...summary(output).keys()]).toEqual([]);
    expect(status).toBe(0);
  });

  test("with both presets on x64 skips only the ARM64 tests, and bundles both builds", () => {
    const { calls, output, status } = run(
      variables(["PRESETS", "windows-arm64,windows-x64"], ["CHECKS", "main"]),
    );
    const reported = [...summary(output).keys()];
    expect(reported).toContain("C++ build: windows-arm64");
    expect(reported).not.toContain("C++ tests: windows-arm64");
    expect(reported).toContain("C++ tests: windows-x64");
    expect(calls.filter((call) => call.startsWith("robocopy"))).toHaveLength(2);
    expect(status).toBe(0);
  });

  test.each(["windows-arm64", "windows-x64"])(
    "bundles nothing when the %s build fails",
    (preset) => {
      const { output } = run(
        variables(
          ["PRESETS", "windows-arm64,windows-x64"],
          ["CHECKS", "main"],
          ["STUB_FAIL", `^cmake --build --preset ${preset} `],
        ),
      );
      expect(summary(output).get("Bundle")).toMatch(/^skipped/);
      expect(summary(output).get("pluginval")).toMatch(/^skipped/);
    },
  );

  // A guard, not a change: what CI's Windows jobs check stays the same.
  const skips = variables(
    ["SKIP_PERFORMANCE", "1"],
    ["SKIP_FORMAT", "1"],
    ["SKIP_APP_TESTS", "1"],
    ["SKIP_TYPESCRIPT", "1"],
  );
  test.each([
    {
      job: "windows-x64",
      checks: "main",
      expected: [
        "C++ build: windows-x64",
        "C++ tests: windows-x64",
        "Bundle",
        "pluginval",
      ],
    },
    {
      job: "windows-x64-glue",
      checks: "glue",
      expected: ["Glue build", "Glue tests"],
    },
  ])(
    "for the $job job runs the stages it ran before",
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
