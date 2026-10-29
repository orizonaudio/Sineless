// What the Bash hooks read from a command: its parts, and the git subcommand each part runs.
import { execFileSync } from "node:child_process";
import process from "node:process";

// The tool call Claude Code passes a PreToolUse hook as JSON on stdin.
export async function bashCommand() {
  let input = "";
  for await (const chunk of process.stdin) {
    input += chunk;
  }
  return JSON.parse(input).tool_input?.command ?? "";
}

// The command cut into the parts the shell runs one after another or in a pipe.
export function parts(command) {
  return command
    .split(/&&|\|\||;|\||\n/)
    .map((part) => part.trim())
    .filter((part) => part !== "");
}

// The git subcommand a part runs, the -C directory it runs in, and the words after the
// subcommand; null for a part that is not git. The subcommand is the first word after `git` that
// is neither an option nor the value of `-C <dir>` or `-c <name>=<value>`, so that
// `git commit -m "... push ..."` and `git stash push` are not pushes.
export function gitCall(part) {
  const words = part.split(/\s+/);
  if (words[0] !== "git") {
    return null;
  }
  let dir;
  let at = 1;
  while (at < words.length && words[at].startsWith("-")) {
    if (words[at] === "-C") {
      dir = words[at + 1];
    }
    at += words[at] === "-C" || words[at] === "-c" ? 2 : 1;
  }
  return { subcommand: words[at], dir, args: words.slice(at + 1) };
}

// The branch checked out in `dir`, or in the working directory; "" when there is none.
export function currentBranch(dir) {
  try {
    return execFileSync(
      "git",
      [...(dir ? ["-C", dir] : []), "rev-parse", "--abbrev-ref", "HEAD"],
      { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] },
    ).trim();
  } catch {
    return "";
  }
}
