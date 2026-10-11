// PreToolUse hook: blocks `git push` to main, so that changes reach main through a pull
// request. Claude Code passes the tool call as JSON on stdin; exit code 2 blocks the call and
// sends stderr back to Claude.
import { execFileSync } from "node:child_process";
import process from "node:process";

let input = "";
for await (const chunk of process.stdin) {
  input += chunk;
}
const command = JSON.parse(input).tool_input?.command ?? "";

const pushes = command
  .split(/&&|\|\||;|\n/)
  .map((part) => part.trim())
  .filter((part) => /^git\s+(?:-C\s+\S+\s+)?push\b/.test(part));

const currentBranch = () => {
  try {
    return execFileSync("git", ["rev-parse", "--abbrev-ref", "HEAD"], {
      encoding: "utf8",
    }).trim();
  } catch {
    return "";
  }
};

for (const push of pushes) {
  const words = push.split(/\s+/).filter((word) => !word.startsWith("-"));
  // After "git push": the remote, then refspecs.
  const refspecs = words.slice(words.indexOf("push") + 2);
  const targetsMain =
    refspecs.length === 0
      ? currentBranch() === "main"
      : refspecs.some(
          (spec) =>
            /(^|:)(refs\/heads\/)?main$/.test(spec) ||
            (spec === "HEAD" && currentBranch() === "main"),
        );
  if (targetsMain) {
    process.stderr.write(
      "Pushing to main is blocked: push a branch and open a pull request instead (see AGENTS.md, Delivering).\n",
    );
    process.exit(2);
  }
}
