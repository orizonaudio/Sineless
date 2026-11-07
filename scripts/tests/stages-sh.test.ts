// The check scripts' stage runner (scripts/stages.sh): every stage runs and the failures are
// reported at the end, so one run of scripts/check.sh shows every finding, not only the first.
import { spawn, spawnSync } from "node:child_process";
import {
  existsSync,
  mkdtempSync,
  openSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { variables } from "./check-harness.ts";

const runner = resolve(import.meta.dirname, "../stages.sh");

let folder: string | undefined;
afterEach(() => {
  if (folder !== undefined) {
    rmSync(folder, { recursive: true, force: true });
    folder = undefined;
  }
});

// A script that sources the runner, as check.sh does, and runs the given stages. It runs in a
// folder of its own, where a stage leaves a marker file to show that it ran.
const script = (body: string) => {
  folder = mkdtempSync(join(tmpdir(), "stages-"));
  const path = join(folder, "check.sh");
  writeFileSync(
    path,
    `set -euo pipefail\ncd "${folder}"\nsource "${runner}"\n${body}\nstages_report\n`,
  );
  return path;
};

const run = (body: string, env: Record<string, string> = {}) => {
  const result = spawnSync("bash", [script(body)], {
    encoding: "utf8",
    env: { ...process.env, ...env },
  });
  return { status: result.status, output: result.stdout + result.stderr };
};

const ran = (marker: string) => existsSync(join(folder ?? "", marker));

describe("a failing stage", () => {
  test("does not stop the stages after it, and the run fails", () => {
    const { status, output } = run(`
      stage One -- false
      stage Two -- touch two
    `);
    expect(ran("two")).toBe(true);
    expect(status).toBe(1);
    expect(output).toContain("1 of 2 stages failed.");
  });

  test("is named in the summary with the command that reruns it alone", () => {
    const { output } = run(`
      stage "macOS lint" --rerun "scripts/lint.sh" -- false
      stage Other -- true
    `);
    const summary = output.slice(output.lastIndexOf("==> Summary"));
    expect(summary).toMatch(/FAILED .*macOS lint/);
    expect(summary).toContain("macOS lint: scripts/lint.sh");
    expect(summary).not.toMatch(/FAILED .*Other/);
  });

  test("stops at its own first failing command", () => {
    run(`
      body() { false; touch after; }
      stage Body -- body
    `);
    expect(ran("after")).toBe(false);
  });

  test("fails when a command in a pipeline fails", () => {
    const { status, output } = run(`
      stage Pipeline -- bash -c 'false | true'
      piped() { false | cat; }
      stage Piped -- piped
    `);
    expect(status).toBe(1);
    expect(output).toContain("1 of 2 stages failed.");
    expect(output).toMatch(/FAILED .*Piped/);
  });
});

describe("a stage that needs another", () => {
  test("is skipped, naming it, when that stage failed", () => {
    const { status, output } = run(`
      stage Build -- false
      stage Tests --needs Build -- touch tests
    `);
    expect(ran("tests")).toBe(false);
    expect(output).toMatch(/skipped .*Tests \(needs Build\)/);
    expect(status).toBe(1);
  });

  test("is skipped when that stage was skipped", () => {
    const { output } = run(`
      stage Build -- false
      stage Bundle --needs Build -- touch bundle
      stage Validate --needs Bundle -- touch validate
    `);
    expect(ran("validate")).toBe(false);
    expect(output).toMatch(/skipped .*Validate \(needs Bundle\)/);
  });

  test("runs when that stage passed", () => {
    run(`
      stage Build -- true
      stage Tests --needs Build -- touch tests
    `);
    expect(ran("tests")).toBe(true);
  });

  test("stops the run when the stage it names does not exist", () => {
    const { status, output } = run(`
      stage Tests --needs Bulid -- touch tests
      stage Later -- touch later
    `);
    expect(status).not.toBe(0);
    expect(ran("tests")).toBe(false);
    expect(ran("later")).toBe(false);
    expect(output).toContain("Bulid");
  });
});

describe("a run", () => {
  test("whose stages all pass succeeds", () => {
    const { status, output } = run(`
      stage One -- true
      stage Two --needs One -- true
    `);
    expect(status).toBe(0);
    expect(output).toContain("All checks passed.");
  });

  test("with no stages succeeds", () => {
    const { status } = run("");
    expect(status).toBe(0);
  });

  test("with BUILD_ONLY=1 runs only the builds", () => {
    const { status, output } = run(
      `
      stage Build --build -- touch build
      stage Tests --needs Build -- touch tests
      stage Lint -- touch lint
    `,
      variables(["BUILD_ONLY", "1"]),
    );
    expect(ran("build")).toBe(true);
    expect(ran("tests")).toBe(false);
    expect(ran("lint")).toBe(false);
    expect(output).toMatch(/not run .*Tests \(builds only\)/);
    expect(status).toBe(0);
  });

  test.each([
    { before: "", when: "nothing failed before" },
    { before: "stage Earlier -- false", when: "an earlier stage failed" },
  ])(
    "stopped with Ctrl-C starts no later stage, when $when",
    async ({ before }) => {
      const path = script(`
      ${before}
      stage Long -- bash -c 'touch started; sleep 20'
      stage Later -- touch later
    `);
      // Ctrl-C signals the terminal's whole foreground process group.
      const output = openSync(join(folder ?? "", "output"), "w");
      const child = spawn("bash", [path], {
        detached: true,
        stdio: ["ignore", output, output],
      });
      const startedAt = Date.now();
      while (!ran("started") && Date.now() - startedAt < 10_000) {
        await new Promise((done) => setTimeout(done, 50));
      }
      process.kill(-(child.pid ?? 0), "SIGINT");
      const status = await new Promise<number | null>((done) =>
        child.on("exit", (code) => {
          done(code);
        }),
      );
      expect(ran("later")).toBe(false);
      expect(status).toBe(130);
      // What was found before the interruption is still reported.
      if (before !== "") {
        expect(readFileSync(join(folder ?? "", "output"), "utf8")).toMatch(
          /Interrupted\.[\s\S]*==> Summary[\s\S]*FAILED .*Earlier/,
        );
      }
    },
  );
});
