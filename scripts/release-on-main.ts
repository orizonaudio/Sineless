// Whether a release's commit is on main: a release is made only from a commit that passed review
// and the merge queue's checks, so never from a tag on a pull request's branch, or on a commit the
// merge queue rebased into another. Exits 1 with the reason if it is not, or if either cannot be
// found (.github/workflows/release.yml, which checks out main's history for it).
//   node scripts/release-on-main.ts <commit> [main, default origin/main]
import { spawnSync } from "node:child_process";

// Whether `commit` is in the history of `main`, in the repository at `cwd`. Throws when git
// cannot tell, as when either is not there: a release must not go ahead on a guess.
export function onMain(cwd: string, commit: string, main: string): boolean {
  const result = spawnSync(
    "git",
    ["merge-base", "--is-ancestor", commit, main],
    { cwd, encoding: "utf8" },
  );
  if (result.status === 0 || result.status === 1) {
    return result.status === 0;
  }
  throw new Error(
    `git cannot tell whether ${commit} is on ${main}: ${result.stderr.trim()}`,
  );
}

if (process.argv[1]?.endsWith("release-on-main.ts") === true) {
  const [commit, main = "origin/main"] = process.argv.slice(2);
  if (commit === undefined) {
    console.error("Usage: node scripts/release-on-main.ts <commit> [main]");
    process.exit(2);
  }
  try {
    if (!onMain(process.cwd(), commit, main)) {
      console.error(
        `${commit.slice(0, 7)} is not on ${main}: tag a commit of main.`,
      );
      process.exit(1);
    }
  } catch (error) {
    console.error((error as Error).message);
    process.exit(1);
  }
}
