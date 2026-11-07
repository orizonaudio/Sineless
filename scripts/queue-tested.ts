// Whether the merge queue already tested a commit pushed to main, for CI's changes job: a push the
// queue tested only fuzzes and fills the compiler caches, since the queue ran every other check on
// it (.github/workflows/ci.yml). Tested means a successful CI run in the queue with the same
// commit, or with the same tree under another commit (the tree is what the checks read). Prints
// true or false; anything it cannot tell, a failed request included, prints false, so that the
// push runs everything.
//   GITHUB_REPOSITORY=owner/repo node scripts/queue-tested.ts <commit>
import { spawnSync } from "node:child_process";

export type Runs = {
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  workflow_runs: {
    // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
    head_sha: string;
    conclusion: string | null;
    // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
    head_commit?: { tree_id?: string } | null;
  }[];
};

export function queueTested(runs: Runs, commit: string, tree: string): boolean {
  return runs.workflow_runs.some(
    (run) =>
      run.conclusion === "success" &&
      (run.head_sha === commit || run.head_commit?.tree_id === tree),
  );
}

const run = (program: string, args: string[]) => {
  const result = spawnSync(program, args, { encoding: "utf8" });
  if (result.status !== 0) {
    throw new Error(`${program} ${args.join(" ")}: ${result.stderr.trim()}`);
  }
  return result.stdout.trim();
};

const queueRuns = (repository: string, filter: string): Runs => {
  const runs = JSON.parse(
    run("gh", [
      "api",
      `repos/${repository}/actions/workflows/ci.yml/runs?event=merge_group&status=success&${filter}`,
    ]),
  ) as Partial<Runs>;
  if (!Array.isArray(runs.workflow_runs)) {
    throw new Error("GitHub's answer has no workflow_runs.");
  }
  return runs as Runs;
};

if (process.argv[1]?.endsWith("queue-tested.ts") === true) {
  const commit = process.argv[2] ?? "";
  try {
    const repository = process.env["GITHUB_REPOSITORY"] ?? "";
    if (commit === "" || repository === "") {
      throw new Error("Needs a commit and GITHUB_REPOSITORY.");
    }
    const sha = run("git", ["rev-parse", commit]);
    const tree = run("git", ["rev-parse", `${commit}^{tree}`]);
    // The commit itself first; then the newest successful queue runs, for the same tree.
    const tested =
      queueTested(queueRuns(repository, `head_sha=${sha}`), sha, tree) ||
      queueTested(queueRuns(repository, "per_page=50"), sha, tree);
    console.log(String(tested));
  } catch (error) {
    console.error(
      `Not known to be tested, so everything runs: ${(error as Error).message}`,
    );
    console.log("false");
  }
}
