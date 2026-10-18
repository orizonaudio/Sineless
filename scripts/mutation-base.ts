// The base to give Mull for a run on the lines changed since a revision (gitDiffRef, in
// scripts/mutation-cpp.sh and scripts/mutation-au.sh). Mull diffs the base's tree with the working
// tree (libgit2's tree-to-workdir diff, which reads no index), and that diff leaves out a file the
// base does not hold: a source file added since the base would get no mutant. So Mull is given a
// commit that holds the base's tree with each such file in the folders mutated added empty, which
// makes every line of it a changed line; with none added, the base itself. The commit is made
// without touching the branches, the index or the working tree, and git collects it as garbage.
// Prints the commit.
//   node scripts/mutation-base.ts <base commit> <folder>...
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

export function mullBase(
  base: string,
  folders: readonly string[],
  cwd = process.cwd(),
): string {
  // Git's own environment variables, by name.
  const git = (args: string[], env: [string, string][] = [], input = "") =>
    execFileSync("git", args, {
      cwd,
      encoding: "utf8",
      env: { ...process.env, ...Object.fromEntries(env) },
      input,
    }).trim();
  // Renames as additions: the new name is as new to Mull's diff as an added file.
  const added = git([
    "diff",
    "--name-only",
    "--no-renames",
    "--diff-filter=A",
    base,
    "--",
    ...folders,
  ])
    .split("\n")
    .filter((path) => path !== "");
  if (added.length === 0) {
    return base;
  }
  const dir = mkdtempSync(join(tmpdir(), "mutation-base-"));
  try {
    const index: [string, string][] = [["GIT_INDEX_FILE", join(dir, "index")]];
    git(["read-tree", base], index);
    const empty = git(["hash-object", "-w", "--stdin"]);
    for (const path of added) {
      git(
        ["update-index", "--add", "--cacheinfo", `100644,${empty},${path}`],
        index,
      );
    }
    const tree = git(["write-tree"], index);
    // Named here: a CI runner's git knows no author.
    const author: [string, string][] = [
      ["GIT_AUTHOR_NAME", "Mutation testing"],
      ["GIT_AUTHOR_EMAIL", "mutation@localhost"],
      ["GIT_COMMITTER_NAME", "Mutation testing"],
      ["GIT_COMMITTER_EMAIL", "mutation@localhost"],
    ];
    return git(
      ["commit-tree", tree, "-p", base],
      author,
      "The base, with the files added since it empty\n",
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

if (process.argv[1]?.endsWith("mutation-base.ts") === true) {
  const [base, ...folders] = process.argv.slice(2);
  if (base === undefined || folders.length === 0) {
    console.error(
      "Usage: node scripts/mutation-base.ts <base commit> <folder>...",
    );
    process.exit(2);
  }
  console.log(mullBase(base, folders));
}
