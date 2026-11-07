// The Windows check's stage runner (scripts/stages.ps1), as scripts/stages.sh for the others:
// every stage runs and the failures are reported at the end. It needs PowerShell, which CI's
// Linux runners have; where it is missing (macOS) these tests are skipped.
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, describe, expect, test } from "vitest";

const runner = resolve(import.meta.dirname, "../stages.ps1");
const pwsh =
  spawnSync("pwsh", ["-NoProfile", "-Command", "exit 0"]).status === 0;

let folder: string | undefined;
afterEach(() => {
  if (folder !== undefined) {
    rmSync(folder, { recursive: true, force: true });
    folder = undefined;
  }
});

// A script that dot-sources the runner, as check.ps1 does, and runs the given stages in a folder
// of its own, where a stage leaves a marker file to show that it ran.
const run = (body: string) => {
  folder = mkdtempSync(join(tmpdir(), "stages-ps1-"));
  const path = join(folder, "check.ps1");
  writeFileSync(
    path,
    `$ErrorActionPreference = 'Stop'
Set-Location '${folder}'
. '${runner}'
function Fail { throw 'failed on purpose' }
${body}
Report-Stages
`,
  );
  const result = spawnSync("pwsh", ["-NoProfile", "-File", path], {
    encoding: "utf8",
  });
  return { status: result.status, output: result.stdout + result.stderr };
};

const ran = (marker: string) => existsSync(join(folder ?? "", marker));

describe.skipIf(!pwsh)("the PowerShell stage runner", () => {
  test("goes on after a failing stage, and the run fails naming it with its rerun", () => {
    const { status, output } = run(`
      Stage -Name 'One' -Rerun 'npm.cmd run lint' -Body { Fail }
      Stage -Name 'Two' -Body { New-Item two | Out-Null }
    `);
    expect(ran("two")).toBe(true);
    expect(output).toMatch(/FAILED .*One/);
    expect(output).toContain("One: npm.cmd run lint");
    expect(output).toContain("1 of 2 stages failed.");
    expect(status).toBe(1);
  });

  test("stops a stage at its own first failure", () => {
    run(`Stage -Name 'Body' -Body { Fail; New-Item after | Out-Null }`);
    expect(ran("after")).toBe(false);
  });

  test("skips a stage whose need failed or was skipped, naming it", () => {
    const { output } = run(`
      Stage -Name 'Build' -Body { Fail }
      Stage -Name 'Bundle' -Needs 'Build' -Body { New-Item bundle | Out-Null }
      Stage -Name 'Validate' -Needs 'Bundle' -Body { New-Item validate | Out-Null }
    `);
    expect(ran("bundle")).toBe(false);
    expect(ran("validate")).toBe(false);
    expect(output).toMatch(/skipped .*Bundle \(needs Build\)/);
    expect(output).toMatch(/skipped .*Validate \(needs Bundle\)/);
  });

  test("stops the run when a need names no stage before it", () => {
    const { status } = run(`
      Stage -Name 'Tests' -Needs 'Bulid' -Body { New-Item tests | Out-Null }
      Stage -Name 'Later' -Body { New-Item later | Out-Null }
    `);
    expect(ran("tests")).toBe(false);
    expect(ran("later")).toBe(false);
    expect(status).not.toBe(0);
  });

  test("passes when every stage passes, and with no stages", () => {
    const passing = run(`
      Stage -Name 'One' -Body { }
      Stage -Name 'Two' -Needs 'One' -Body { New-Item two | Out-Null }
    `);
    expect(ran("two")).toBe(true);
    expect(passing.output).toContain("All checks passed.");
    expect(passing.status).toBe(0);
    expect(run("").status).toBe(0);
  });
});
