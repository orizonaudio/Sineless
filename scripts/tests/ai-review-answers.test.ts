// The AI review's verifiers see the author's answers to earlier findings, so that an answered
// finding is not judged afresh every round (scripts/ai-review-answers.ts). The fixtures are #287's
// real comments: the bot's inline findings, the author's replies, the bot's summaries and
// /ai-review commands.
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  copyFileSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import { collect } from "../ai-review-answers.ts";

const root = join(import.meta.dirname, "..", "..");
const fixtures = join(import.meta.dirname, "fixtures", "ai-review-answers");
const read = (name: string): unknown =>
  JSON.parse(readFileSync(join(fixtures, name), "utf8"));
const review = read("review-comments.json") as Record<string, unknown>[];
const conversation = read("conversation.json") as Record<string, unknown>[];

const importantRoot = 4407589277;
const unanswered = 4407858733;
const byId = (id: number) => review.find((c) => c["id"] === id) ?? {};
const answerTo = (
  id: number,
  association: string,
  body = "Not tested, on purpose.",
) => ({
  id: id + 1,
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  in_reply_to_id: id,
  user: { login: "someone" },
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  author_association: association,
  body,
});

describe("the author's answers", () => {
  test("include a reply to an Important finding, with where the finding was", () => {
    const { threads } = collect(review, []);
    expect(threads).toHaveLength(1);
    expect(threads[0]?.file).toBe("engine/src/Analyzer.cpp");
    // Outdated since: the line it was on.
    expect(threads[0]?.line).toBe(1299);
    expect(threads[0]?.finding).toMatch(/^placingStamp stops following/u);
    expect(threads[0]?.replies[0]?.body).toMatch(/^Confirmed with 64-sample/u);
  });

  test("leave out replies to nits and Important findings no one answered", () => {
    const findings = collect(review, []).threads.map((t) => t.finding);
    expect(findings.some((f) => f.includes("kOffsetPersistence"))).toBe(false);
    expect(byId(unanswered)["body"]).toMatch(/^\*\*Important:\*\*/u);
    expect(findings.some((f) => f.startsWith("No test triggers"))).toBe(false);
  });

  test("keep a finding on a whole file, without a line", () => {
    const fileLevel: Record<string, unknown> = {
      ...byId(importantRoot),
      line: null,
    };
    fileLevel["original_line"] = null;
    const { threads } = collect(
      [fileLevel, ...review.filter((c) => c["id"] !== importantRoot)],
      [],
    );
    expect(threads[0]?.line).toBeNull();
  });

  // Every association GitHub gives; only the owner, members and collaborators answer for it.
  test.each([
    ["OWNER", true],
    ["MEMBER", true],
    ["COLLABORATOR", true],
    ["CONTRIBUTOR", false],
    ["FIRST_TIME_CONTRIBUTOR", false],
    ["FIRST_TIMER", false],
    ["MANNEQUIN", false],
    ["NONE", false],
  ])(
    "count a reply from an author who is %s only from the owner, members and collaborators (%s)",
    (association, counts) => {
      const reply = answerTo(unanswered, association);
      const threads = collect([...review, reply], []).threads;
      const answered = threads.some((t) =>
        t.finding.startsWith("No test triggers"),
      );
      expect(answered).toBe(counts);
      const comments = collect(
        [],
        [{ ...reply, body: "The rate cannot differ." }],
      ).comments;
      expect(comments.length > 0).toBe(counts);
    },
  );

  test("include the author's own comments, not the bot's or review commands", () => {
    expect(collect([], conversation).comments).toEqual([]);
    const own = answerTo(
      0,
      "MEMBER",
      "Both findings are answered in the threads.",
    );
    const quoting = answerTo(0, "MEMBER", "Waiting before another /ai-review.");
    expect(
      collect([], [...conversation, own, quoting]).comments.map((c) => c.body),
    ).toEqual([own.body, quoting.body]);
  });

  test.each([
    ["no comments", []],
    ["no pages", null],
  ])("are none for a pull request with %s", (_, value) => {
    expect(collect(value, value)).toEqual({ threads: [], comments: [] });
  });
});

// The workflow step that writes the answers for the verifiers, run as the job runs it, with a fake
// gh: whatever fails, it leaves an empty list and the review goes on.
describe("the review's answers step", () => {
  const workflow = readFileSync(
    join(root, ".github", "workflows", "ai-review.yml"),
    "utf8",
  );
  const script =
    /- name: Collect the author's answers\n[\s\S]*?run: \|\n((?: {10}.*\n|\n)+)/u
      .exec(workflow)?.[1]
      ?.replace(/^ {10}/gmu, "") ?? "";

  function runStep(
    gh: string,
    withScript = true,
  ): { code: number | null; answers: unknown } {
    const dir = mkdtempSync(join(tmpdir(), "ai-review-answers-"));
    mkdirSync(join(dir, "ai-review"));
    mkdirSync(join(dir, "bin"));
    if (withScript) {
      mkdirSync(join(dir, "scripts"));
      copyFileSync(
        join(root, "scripts", "ai-review-answers.ts"),
        join(dir, "scripts", "ai-review-answers.ts"),
      );
    }
    writeFileSync(join(dir, "bin", "gh"), `#!/bin/sh\n${gh}\n`);
    chmodSync(join(dir, "bin", "gh"), 0o755);
    const result = spawnSync("bash", ["-eo", "pipefail", "-c", script], {
      cwd: dir,
      encoding: "utf8",
      env: {
        ...process.env,
        PATH: `${join(dir, "bin")}:${process.env["PATH"] ?? ""}`,
        REPO: "o/r",
        NUMBER: "287",
      },
    });
    return {
      code: result.status,
      answers: JSON.parse(
        readFileSync(join(dir, "ai-review", "answers.json"), "utf8"),
      ),
    };
  }

  const none = { threads: [], comments: [] };
  const pages = (file: string) => `cat ${JSON.stringify(join(fixtures, file))}`;
  const answeringGh = `case "$2" in *pulls*) ${pages("review-comments.json")};; *) ${pages("conversation.json")};; esac`;

  test("is read from the workflow", () => {
    expect(script).toContain("ai-review/answers.json");
  });

  test("writes the answers for the verifiers", () => {
    const { code, answers } = runStep(answeringGh);
    expect(code).toBe(0);
    expect(answers).toEqual(collect(review, conversation));
  });

  test.each([
    ["GitHub cannot be asked", "exit 1"],
    ["GitHub's answer cannot be read", "echo 'not json'"],
  ])("goes on without answers when %s", (_, gh) => {
    expect(runStep(gh)).toEqual({ code: 0, answers: none });
  });

  test("goes on without answers on a base branch without the script", () => {
    expect(runStep(answeringGh, false)).toEqual({ code: 0, answers: none });
  });
});
