// An agent session cannot push to, re-review or hand back a pull request that waits for a
// maintainer (labelled needs-human), nor push while its AI review is still running
// (.claude/hooks/review-stop.mjs). The hook runs as Claude Code runs it: the tool call as JSON on
// stdin, exit code 2 to block, in a git checkout, with a fake `gh` first on PATH that answers from
// files and records its calls.
import { execFileSync, spawnSync } from "node:child_process";
import {
  chmodSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { beforeAll, beforeEach, describe, expect, test } from "vitest";

const root = join(import.meta.dirname, "..", "..");
const hook = join(root, ".claude", "hooks", "review-stop.mjs");
const noPushToMain = join(root, ".claude", "hooks", "no-push-to-main.mjs");

const labelled = "claude/labelled";
const clean = "clean";
const reviewing = "reviewing";

interface PullRequest {
  number: number;
  headRefName: string;
  headRefOid: string;
  labels: { name: string }[];
}

const pullRequests: PullRequest[] = [
  {
    number: 287,
    headRefName: labelled,
    headRefOid: "sha287",
    labels: [{ name: "needs-human" }],
  },
  { number: 288, headRefName: clean, headRefOid: "sha288", labels: [] },
  {
    number: 290,
    headRefName: reviewing,
    headRefOid: "sha290",
    labels: [{ name: "bug" }],
  },
  // A branch whose name contains the labelled one, and a number that contains its number.
  {
    number: 2870,
    headRefName: `${labelled}-2`,
    headRefOid: "sha2870",
    labels: [],
  },
];

const status = (state: string, minutesAgo = 0) => ({
  context: "ai-review",
  state,
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's status API names it.
  created_at: new Date(Date.now() - minutesAgo * 60_000).toISOString(),
});

let repo = "";
let other = "";
let mainCheckout = "";
let fakes = "";

const git = (cwd: string, ...args: string[]) =>
  execFileSync("git", args, { cwd, encoding: "utf8" }).trim();

beforeAll(() => {
  const base = mkdtempSync(join(tmpdir(), "review-stop-"));
  repo = join(base, "repo");
  // A path with a space in it, as this repository's own is.
  other = join(base, "other checkout");
  fakes = join(base, "fakes");
  execFileSync("mkdir", ["-p", repo, fakes]);
  git(repo, "init", "-q", "-b", "nopr");
  git(
    repo,
    "-c",
    "user.name=t",
    "-c",
    "user.email=t@t",
    "commit",
    "-q",
    "--allow-empty",
    "-m",
    "start",
  );
  for (const branch of [labelled, clean, reviewing]) {
    git(repo, "branch", branch);
  }
  // A second checkout, on the labelled branch (a clone: a worktree would hold the branch).
  git(base, "clone", "-q", "-b", labelled, repo, other);
  // And one on main.
  mainCheckout = join(base, "main checkout");
  git(base, "clone", "-q", repo, mainCheckout);
  git(mainCheckout, "checkout", "-q", "-b", "main");

  // The fake gh: `pr list` prints the open pull requests, `api .../commits/<sha>/statuses` the
  // statuses written for that commit; a file named fail-list or fail-status makes that call fail.
  const gh = join(fakes, "gh");
  writeFileSync(
    gh,
    `#!/usr/bin/env node
const { appendFileSync, existsSync, readFileSync } = require("node:fs");
const { join } = require("node:path");
const dir = ${JSON.stringify(fakes)};
const args = process.argv.slice(2);
appendFileSync(join(dir, "calls.log"), JSON.stringify(args) + "\\n");
if (args[0] === "pr" && args[1] === "list") {
  if (existsSync(join(dir, "fail-list"))) process.exit(1);
  process.stdout.write(readFileSync(join(dir, "prs.json")));
  process.exit(0);
}
const sha = /commits\\/([^/]+)\\/statuses/.exec(args.join(" "))?.[1];
if (args[0] === "api" && sha) {
  if (existsSync(join(dir, "fail-status"))) process.exit(1);
  const file = join(dir, "statuses-" + sha + ".json");
  process.stdout.write(existsSync(file) ? readFileSync(file) : "[]");
  process.exit(0);
}
process.exit(1);
`,
  );
  chmodSync(gh, 0o755);
});

function setStatuses(sha: string, statuses: object[]): void {
  writeFileSync(join(fakes, `statuses-${sha}.json`), JSON.stringify(statuses));
}

beforeEach(() => {
  writeFileSync(join(fakes, "prs.json"), JSON.stringify(pullRequests));
  writeFileSync(join(fakes, "calls.log"), "");
  execFileSync("rm", [
    "-f",
    join(fakes, "fail-list"),
    join(fakes, "fail-status"),
  ]);
  // The labelled and clean pull requests' reviews have finished; 290's is running.
  setStatuses("sha287", [status("failure"), status("pending", 8)]);
  setStatuses("sha288", [status("success"), status("pending", 8)]);
  setStatuses("sha290", [status("pending", 3)]);
  setStatuses("sha2870", [status("success")]);
  git(repo, "checkout", "-q", "nopr");
});

function run(
  script: string,
  command: string,
  branch = "nopr",
): { code: number | null; stderr: string } {
  git(repo, "checkout", "-q", branch);
  const result = spawnSync("node", [script], {
    cwd: repo,
    // eslint-disable-next-line @typescript-eslint/naming-convention -- Claude Code names it.
    input: JSON.stringify({ tool_input: { command } }),
    encoding: "utf8",
    env: { ...process.env, PATH: `${fakes}:${process.env["PATH"] ?? ""}` },
  });
  return { code: result.status, stderr: result.stderr };
}

const calls = () =>
  readFileSync(join(fakes, "calls.log"), "utf8").split("\n").filter(Boolean);

const blocked = (command: string, branch = "nopr", pr = 287) => {
  const { code, stderr } = run(hook, command, branch);
  expect(code, stderr).toBe(2);
  expect(stderr).toContain(`#${String(pr)}`);
};
const allowed = (command: string, branch = "nopr") => {
  const { code, stderr } = run(hook, command, branch);
  expect(code, stderr).toBe(0);
};

// Each way a push can name a branch, from a checkout on another branch.
const named = (branch: string) => [
  `git push origin ${branch}`,
  `git push origin HEAD:${branch}`,
  `git push origin +${branch}`,
  `git push origin refs/heads/${branch}`,
  `git push origin HEAD:refs/heads/${branch}`,
];
const flags = ["", " -u", " --force-with-lease"];
const withFlags = (commands: string[]) =>
  commands.flatMap((command) =>
    flags.map((flag) => command.replace("git push", `git push${flag}`)),
  );

describe("an agent on a pull request that needs a maintainer", () => {
  test.each(withFlags(named(labelled)))(
    "cannot push to it by naming its branch: %s",
    (command) => {
      blocked(command);
      blocked(`${command} 2>&1 | tail -1`);
    },
  );

  test.each(withFlags(named(clean)))(
    "can push to another pull request's branch: %s",
    (command) => {
      allowed(command);
    },
  );

  test.each(["git push", "git push origin", "git push -u origin HEAD"])(
    "cannot push its checked-out branch: %s",
    (command) => {
      blocked(command, labelled);
      blocked(`git add -A && ${command}`, labelled);
    },
  );

  test("cannot push from a checkout of it named with -C", () => {
    blocked(`git -C "${other}" push`);
    blocked(`git -C '${other}' push origin HEAD`);
    blocked(`git -c push.default=current -C "${other}" push`);
  });

  test("can push to an unlabelled branch named with -C from its own checkout", () => {
    const { code } = run(hook, `git -C ${repo} push`, clean);
    expect(code).toBe(0);
  });

  test("cannot push elsewhere from its checked-out branch, and is told why", () => {
    const { code, stderr } = run(hook, `git push origin ${clean}`, labelled);
    expect(code).toBe(2);
    expect(stderr).toContain("checked out");
  });

  test.each([
    `gh pr comment 287 --body "/ai-review"`,
    `gh pr comment '#287' --body /ai-review`,
    `gh api repos/o/r/issues/287/comments -f body=/ai-review`,
    `gh pr comment ${labelled} --body /ai-review`,
    `echo /ai-review | gh pr comment 287 -F -`,
    `gh pr comment 287 --body "$(cat <<'EOF'\nFixed.\n/ai-review\nEOF\n)"`,
  ])("cannot ask for another review: %s", (command) => {
    blocked(command);
  });

  test("cannot ask for another review of its checked-out branch without naming it", () => {
    blocked(`gh pr comment --body /ai-review`, labelled);
  });

  test.each([
    `gh pr comment 288 --body /ai-review`,
    `gh pr comment 2870 --body /ai-review`,
    `gh api repos/o/r/issues/2870/comments -f body=/ai-review`,
    `gh pr comment 28 --body /ai-review`,
  ])(
    "can ask for a review of another pull request from its branch: %s",
    (command) => {
      // 28 is no open pull request, so the checked-out branch's would be the fallback; from an
      // unlabelled branch it is allowed.
      allowed(command, command.includes(" 28 ") ? clean : labelled);
    },
  );

  test(`can push to a branch whose name contains its branch's`, () => {
    allowed(`git push origin ${labelled}-2`);
  });

  test("can push once the pull request is merged and the label stays", () => {
    writeFileSync(
      join(fakes, "prs.json"),
      JSON.stringify(pullRequests.filter((pr) => pr.number !== 287)),
    );
    allowed(`git push origin ${labelled}`);
  });
});

describe("an agent never hands a pull request back or waives its review", () => {
  test.each([
    "gh pr edit 288 --remove-label needs-human",
    "gh issue edit 288 --remove-label needs-human",
    "gh api -X DELETE repos/o/r/issues/288/labels/needs-human",
    "gh api --method DELETE repos/o/r/issues/288/labels/needs-human",
    "gh api --method=DELETE repos/o/r/issues/288/labels/needs-human",
    "gh pr edit 288 \\\n  --remove-label needs-human",
    "gh pr comment 288 --body '/ai-review waive the findings do not hold'",
    `gh pr comment 288 --body "$(cat <<'EOF'\n/ai-review waive the findings do not hold\nEOF\n)"`,
    `echo "/ai-review waive x" | gh pr comment 288 -F -`,
  ])("%s is blocked on any pull request", (command) => {
    const { code } = run(hook, command, clean);
    expect(code).toBe(2);
  });

  test.each([
    "gh pr edit 288 --remove-label bug",
    `gh pr comment 288 --body "Waiting for needs-human to be removed"`,
  ])("%s is allowed", (command) => {
    allowed(command, clean);
  });
});

describe("an agent on a pull request that needs a maintainer can still read", () => {
  test.each([
    "gh run list --workflow .github/workflows/ai-review.yml",
    "gh api repos/o/r/contents/.github/workflows/ai-review.yml",
  ])("%s", (command) => {
    allowed(command, labelled);
  });
});

describe("an agent waits for the AI review of its last push", () => {
  test.each([
    `git push origin ${reviewing}`,
    `gh pr comment 290 --body /ai-review`,
  ])("while it runs: %s is blocked, with how long it has run", (command) => {
    const { code, stderr } = run(hook, command);
    expect(code).toBe(2);
    expect(stderr).toContain("#290");
    expect(stderr).toContain("3 minutes");
  });

  test("while a re-review of the same head runs, above its earlier result", () => {
    setStatuses("sha290", [
      status("pending", 1),
      status("failure", 9),
      status("pending", 20),
    ]);
    const { code } = run(hook, `git push origin ${reviewing}`);
    expect(code).toBe(2);
  });

  test.each([
    ["failed", [status("failure"), status("pending", 8)]],
    ["passed", [status("success"), status("pending", 8)]],
    ["not started", []],
  ])("not once it has %s", (_, statuses) => {
    setStatuses("sha290", statuses);
    allowed(`git push origin ${reviewing}`);
  });
});

describe("the hook stays out of the way", () => {
  test.each([
    "git status",
    "gh pr view 287",
    `git commit -m "Block the push while the review runs"`,
    "git stash push -m wip",
    "git log --grep push",
  ])("%s is allowed and never asks GitHub", (command) => {
    setStatuses("sha287", [status("pending")]);
    allowed(command, labelled);
    expect(calls()).toEqual([]);
  });

  test.each(["fail-list", "fail-status"])(
    "a push goes ahead when GitHub cannot be asked (%s)",
    (failure) => {
      writeFileSync(join(fakes, failure), "");
      allowed(`git push origin ${reviewing}`);
    },
  );

  test("a push goes ahead when GitHub's answer cannot be read", () => {
    writeFileSync(join(fakes, "prs.json"), "not json");
    allowed(`git push origin ${labelled}`);
  });

  test("the hook runs on every Bash call", () => {
    const settings = JSON.parse(
      readFileSync(join(root, ".claude", "settings.json"), "utf8"),
    ) as {
      hooks: {
        // eslint-disable-next-line @typescript-eslint/naming-convention -- Claude Code names it.
        PreToolUse: { matcher: string; hooks: { command: string }[] }[];
      };
    };
    const commands = settings.hooks.PreToolUse.filter(
      (entry) => entry.matcher === "Bash",
    ).flatMap((entry) => entry.hooks.map((h) => h.command));
    expect(commands.some((c) => c.includes("review-stop.mjs"))).toBe(true);
    expect(existsSync(hook)).toBe(true);
  });
});

describe("pushing to main stays blocked", () => {
  test.each([
    ["git push origin main", "nopr"],
    ["git push origin HEAD:main", "nopr"],
    ["git push origin HEAD:refs/heads/main", "nopr"],
    ["git push", "main"],
    ["git push origin HEAD", "main"],
    ["git push origin main 2>&1 | tail -1", "nopr"],
    ['git -C "/a path/with spaces" push origin main', "nopr"],
  ])("%s on %s is blocked", (command, branch) => {
    if (branch === "main") {
      git(repo, "branch", "-f", "main");
    }
    expect(run(noPushToMain, command, branch).code).toBe(2);
  });

  test.each(["push", "push origin HEAD"])(
    "git -C <a checkout on main> %s from another branch is blocked",
    (args) => {
      expect(run(noPushToMain, `git -C "${mainCheckout}" ${args}`).code).toBe(
        2,
      );
    },
  );

  test.each([
    `git push origin ${clean}`,
    `git push origin ${clean}:other`,
    `git -C "${other}" push`,
    `git commit -m "never push to main"`,
  ])("%s is allowed", (command) => {
    expect(run(noPushToMain, command).code).toBe(0);
  });
});
