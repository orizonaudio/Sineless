// PreToolUse hook: blocks `git push` to main, so that changes reach main through a pull
// request. Claude Code passes the tool call as JSON on stdin; exit code 2 blocks the call and
// sends stderr back to Claude.
import process from "node:process";

import { bashCommand, currentBranch, gitCall, parts } from "./commands.mjs";

const pushes = parts(await bashCommand())
  .map(gitCall)
  .filter((call) => call?.subcommand === "push");

for (const push of pushes) {
  // After "push": the remote, then refspecs.
  const refspecs = push.args.filter((word) => !word.startsWith("-")).slice(1);
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
      "Pushing to main is blocked: push a branch and open a pull request instead (see CLAUDE.md, Delivering).\n",
    );
    process.exit(2);
  }
}
