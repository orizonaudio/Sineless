// PreToolUse hook: stops an agent session from pushing to, asking for another AI review of, or
// handing back a pull request that waits for a maintainer (labelled needs-human), and from pushing
// while the AI review of its last push is still running (CLAUDE.md, Delivering). Exit code 2
// blocks the call and sends stderr back to Claude. The owner's own terminal runs no hooks.
//
// Where a push goes is not worked out from its refspec: the open pull requests are listed, and
// each whose branch a word of the command names, or whose branch is checked out where the push
// runs, is a target. When GitHub cannot be asked, the call goes ahead: a hook that blocks on a
// network error would stop all work, and the push itself would fail offline.
import { execFileSync } from "node:child_process";
import process from "node:process";

import { bashCommand, currentBranch, gitCall, parts } from "./commands.mjs";

const label = "needs-human";
const command = await bashCommand();
const split = parts(command);
const ghParts = split.filter((part) => /^gh\s/.test(part));

function block(message) {
  process.stderr.write(`${message}\n`);
  process.exit(2);
}

// A comment's text can sit on another part than its gh (a heredoc, `echo ... | gh ...`), so the
// whole command is read for it. A path such as .github/workflows/ai-review.yml is not a command.
const runsGh = ghParts.length > 0;
const reviewCommand = /(?<![\w./-])\/ai-review(?![\w.-])/i;

// Only a maintainer hands a pull request back or decides that its findings do not hold.
if (runsGh && /(?<![\w./-])\/ai-review\s+waive/i.test(command)) {
  block(
    "Waiving the AI review is a maintainer's decision: give the owner the findings and your recommendation.",
  );
}
if (
  runsGh &&
  command.includes(label) &&
  /--remove-label|(-X|--method)[\s=]*DELETE/i.test(command)
) {
  block(
    `Only the owner removes the ${label} label: it is how a pull request is handed back (CLAUDE.md, Delivering).`,
  );
}

const pushes = split.map(gitCall).filter((call) => call?.subcommand === "push");
const asksForReview = runsGh && reviewCommand.test(command);
if (pushes.length === 0 && !asksForReview) {
  process.exit(0);
}

function gh(...args) {
  try {
    return JSON.parse(
      execFileSync("gh", args, {
        encoding: "utf8",
        stdio: ["ignore", "pipe", "ignore"],
      }),
    );
  } catch {
    return null;
  }
}

const open = gh(
  "pr",
  "list",
  "--state",
  "open",
  "--json",
  "number,headRefName,headRefOid,labels",
  "--limit",
  "200",
);
if (!Array.isArray(open)) {
  process.exit(0);
}

const unquoted = (word) => word.replace(/^["']+|["']+$/g, "");
// A push names a branch as `x`, `HEAD:x`, `+x`, `refs/heads/x` or `HEAD:refs/heads/x`.
const pushedBranches = (words) =>
  words.flatMap((word) =>
    unquoted(word)
      .split(":")
      .map((name) => name.replace(/^\+/, "").replace(/^refs\/heads\//, "")),
  );
const onBranch = (branch) => open.filter((pr) => pr.headRefName === branch);

// Each target with why it is one: named by the command, or checked out where it runs.
const targets = [];
for (const push of pushes) {
  const names = pushedBranches(push.args);
  for (const pr of open.filter((pr) => names.includes(pr.headRefName))) {
    targets.push({ pr, why: "" });
  }
  for (const pr of onBranch(currentBranch(push.dir))) {
    targets.push({
      pr,
      why: ` (its branch is checked out where the push runs)`,
    });
  }
}
if (asksForReview) {
  const words = command.split(/\s+/).map(unquoted);
  const numbers = new Set(command.split(/[\s/#"'=]+/));
  const named = open.filter(
    (pr) => numbers.has(String(pr.number)) || words.includes(pr.headRefName),
  );
  for (const pr of named.length > 0 ? named : onBranch(currentBranch())) {
    targets.push({ pr, why: "" });
  }
}

for (const { pr, why } of targets) {
  if (pr.labels.some((l) => l.name === label)) {
    block(
      `PR #${pr.number} needs a maintainer${why}: give the owner its open findings with a recommendation (fix, redesign or waive) and wait; the owner removes the ${label} label to hand it back (CLAUDE.md, Delivering).`,
    );
  }
}
for (const { pr, why } of targets) {
  // Newest first: a reviewed head keeps the review's earlier pending under its result.
  const statuses = gh(
    "api",
    `repos/{owner}/{repo}/commits/${pr.headRefOid}/statuses`,
  );
  const newest = Array.isArray(statuses)
    ? statuses.find((s) => s.context === "ai-review")
    : undefined;
  if (newest?.state === "pending") {
    const minutes = Math.round(
      (Date.now() - Date.parse(newest.created_at)) / 60_000,
    );
    block(
      `The AI review of PR #${pr.number}${why} (${pr.headRefOid.slice(0, 7)}) has been running for ${minutes} minutes: wait for it, and for CI's result, before pushing or asking again. If it has run longer than the review job's timeout, it is stuck: ask the owner.`,
    );
  }
}
