// The AI review's Find step runs each specialist on the model and effort its definition in
// .claude/agents names: CLAUDE_CODE_EFFORT_LEVEL overrides a definition's effort, so a change to
// one side alone would silently run the specialists on the other. The Verify step runs the
// verifiers on settings of its own and is not compared.
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

const root = join(import.meta.dirname, "..", "..");
const workflow = readFileSync(
  join(root, ".github", "workflows", "ai-review.yml"),
  "utf8",
);

// Every specialist the workflow can run, from its specialists= lines.
const specialists = [
  ...new Set(
    [...workflow.matchAll(/specialists\+?="([^"]+)"/gu)].flatMap((match) =>
      (match[1] ?? "").trim().split(/\s+/u),
    ),
  ),
].sort();

// The Find step, up to the next step.
const findStep = /- name: Find\n([\s\S]*?)\n {6}- /u.exec(workflow)?.[1] ?? "";
const findEnv = (name: string) =>
  new RegExp(`^\\s+${name}: (\\S+)$`, "mu").exec(findStep)?.[1];

// A field of an agent definition's front matter.
function field(agent: string, name: string): string | undefined {
  const definition = readFileSync(
    join(root, ".claude", "agents", `${agent}.md`),
    "utf8",
  );
  const frontMatter = /^---\n([\s\S]*?)\n---/u.exec(definition)?.[1] ?? "";
  return new RegExp(`^${name}: (\\S+)$`, "mu").exec(frontMatter)?.[1];
}

describe("the AI review's specialists", () => {
  test("are read from the workflow, with its Find step", () => {
    expect(specialists).toContain("review-correctness");
    expect(findStep).toContain("--model");
  });

  test.each(specialists)(
    "%s names a full model ID and an effort of its own",
    (agent) => {
      expect(field(agent, "model")).toMatch(/^claude-[a-z]+-\d+-\d+$/u);
      expect(field(agent, "effort")).toBeDefined();
    },
  );

  test.each(specialists)(
    "%s runs in CI on the model and effort its definition names",
    (agent) => {
      expect(findEnv("CLAUDE_CODE_SUBAGENT_MODEL")).toBe(field(agent, "model"));
      expect(findEnv("CLAUDE_CODE_EFFORT_LEVEL")).toBe(field(agent, "effort"));
    },
  );
});
