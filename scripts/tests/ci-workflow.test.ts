// What CI runs on a push to main that the merge queue already tested (.github/workflows/ci.yml):
// fuzzing, and the builds that fill main's compiler caches, which only main's runs save; every
// other job skips, since the queue ran it on the same commit. Read from the workflow's text, job
// by job, so that a job added later is held to the same rules.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, test } from "vitest";

const workflow = readFileSync(
  resolve(import.meta.dirname, "../../.github/workflows/ci.yml"),
  "utf8",
);

// Each job's lines, by its key under jobs:.
const jobs = new Map<string, string>();
{
  const lines = workflow.slice(workflow.indexOf("\njobs:\n")).split("\n");
  let name: string | undefined;
  for (const line of lines) {
    const key = /^ {2}([a-z0-9-]+):$/.exec(line);
    if (key) {
      name = key[1] ?? "";
      jobs.set(name, "");
    } else if (name !== undefined) {
      jobs.set(name, `${jobs.get(name) ?? ""}${line}\n`);
    }
  }
}
const job = (name: string) => jobs.get(name) ?? "";

const tested = "needs.changes.outputs.tested";
const buildOnly = `BUILD_ONLY: \${{ ${tested} == 'true' && '1' || '' }}`;

// The jobs that run on a push to main at all (not the pull-request-only ones, nor those that
// need one).
const pullRequestOnly = (name: string): boolean => {
  const text = job(name);
  if (text.includes("github.event_name == 'pull_request'")) {
    return true;
  }
  const needs = /^ {4}needs: \[?([^\]\n]+)\]?$/m.exec(text)?.[1] ?? "";
  return needs
    .split(",")
    .map((need) => need.trim())
    .some(
      (need) => need !== "changes" && jobs.has(need) && pullRequestOnly(need),
    );
};
const onPush = [...jobs.keys()].filter((name) => !pullRequestOnly(name));

// The jobs that save a compiler cache and run a check script: on a tested push they build alone.
const cacheBuilders = ["linux", "linux-checks", "macos", "macos-glue"];
// Run on every push: what decides, fuzzing (it adds to main's corpus), and the gate.
const always = ["changes", "fuzz", "engine-work"];

describe("a push to main the merge queue tested", () => {
  test("is told apart by the changes job, from the queue's runs", () => {
    expect(job("changes")).toMatch(
      /tested: \$\{\{ steps\.\S+\.outputs\.tested \}\}/,
    );
    expect(job("changes")).toContain("node scripts/queue-tested.ts");
    expect(job("changes")).toMatch(
      /permissions:\n\s+actions: read\n\s+contents: read/,
    );
  });

  test.each(cacheBuilders)("runs only the builds in %s", (name) => {
    expect(job(name)).toContain(buildOnly);
    expect(job(name)).toMatch(/save: .*SAVE_CACHES/);
  });

  test.each(
    onPush.filter(
      (name) => !always.includes(name) && !cacheBuilders.includes(name),
    ),
  )("skips %s, by its condition or its steps'", (name) => {
    const condition = /^ {4}if: (.+)$/m.exec(job(name))?.[1] ?? "";
    const stepGate = new RegExp(
      `^ {6}[A-Z]+: .*${tested.replaceAll(".", "\\.")} != 'true'`,
      "m",
    );
    expect(
      condition.includes(`${tested} != 'true'`) || stepGate.test(job(name)),
      `${name}: ${condition}`,
    ).toBe(true);
  });

  test("still fuzzes", () => {
    expect(job("fuzz")).not.toContain(tested);
  });

  test("skips the lint leg of linux-checks and keeps the sanitizers' builds", () => {
    expect(job("linux-checks")).toMatch(
      new RegExp(`${tested} != 'true' \\|\\| matrix\\.check != 'lint'`),
    );
  });
});
