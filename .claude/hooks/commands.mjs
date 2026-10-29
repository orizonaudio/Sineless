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

// The command cut into the parts the shell runs one after another or in a pipe; a line ending in
// a backslash continues on the next.
export function parts(command) {
  return command
    .replace(/\\\n/g, " ")
    .split(/&&|\|\||;|\||\n/)
    .map((part) => part.trim())
    .filter((part) => part !== "");
}

// A part's words as the shell reads them: quotes group words and are removed, so that
// `git -C "/a path/with spaces" push` names one directory.
export function words(part) {
  return [...part.matchAll(/(?:[^\s"']+|"[^"]*"|'[^']*')+/g)].map((match) =>
    match[0].replace(/"([^"]*)"|'([^']*)'/g, "$1$2"),
  );
}

// The git subcommand a part runs, the -C directory it runs in, and the words after the
// subcommand; null for a part that is not git. The subcommand is the first word after `git` that
// is neither an option nor the value of `-C <dir>` or `-c <name>=<value>`, so that
// `git commit -m "... push ..."` and `git stash push` are not pushes.
export function gitCall(part) {
  const all = words(part);
  if (all[0] !== "git") {
    return null;
  }
  let dir;
  let at = 1;
  while (at < all.length && all[at].startsWith("-")) {
    if (all[at] === "-C") {
      dir = all[at + 1];
    }
    at += all[at] === "-C" || all[at] === "-c" ? 2 : 1;
  }
  return { subcommand: all[at], dir, args: all.slice(at + 1) };
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
