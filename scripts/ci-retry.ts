// What to do with a failed CI run (.github/workflows/ci-retry.yml): when every job that failed
// never got a runner ("not acquired by Runner ... after multiple attempts": cancelled, with no
// runner and no step run), nothing in the change failed. A pull request's or main's run is then
// rerun once. A merge-queue run gets a comment on its pull request instead, for the session that
// owns it to queue it again: a queue entry the workflow's own token made would never start CI
// (GitHub's "Triggering a workflow from a workflow"). Any job that ran and failed, or a second
// attempt, and the run is left as it is.
//   GITHUB_REPOSITORY=owner/repo node scripts/ci-retry.ts <run id>
import { appendFileSync } from "node:fs";
import { spawnSync } from "node:child_process";

export type Job = {
  name: string;
  conclusion: string | null;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  runner_name?: string | null;
  steps: { name: string; conclusion: string | null }[];
};

export type Run = {
  id: number;
  event: string;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  run_attempt: number;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  head_branch: string;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  head_sha: string;
  conclusion: string | null;
};

export type Decision = {
  action: "rerun" | "comment" | "none";
  reason: string;
  jobs: string[];
  pullRequest?: number;
};

const failed = (job: Job) =>
  ["failure", "cancelled", "timed_out", "startup_failure"].includes(
    job.conclusion ?? "",
  );

// A job that never got a runner: no runner named, and no step past the runner's own set-up.
export const neverRan = (job: Job) =>
  !job.runner_name &&
  !job.steps.some(
    (step) => step.name !== "Set up job" && step.conclusion !== null,
  );

// `head`, for a pull request's run, is its branch's commit now: a run a newer push replaced is not
// rerun, since the rerun would join the pull request's CI group and cancel the newer run.
export function decide(run: Run, jobs: Job[], head?: string): Decision {
  const bad = jobs.filter(failed);
  const names = bad.map((job) => job.name);
  const none = (reason: string): Decision => ({
    action: "none",
    reason,
    jobs: names,
  });
  if (run.conclusion !== "failure") {
    return none(`the run's conclusion is ${run.conclusion ?? "none"}`);
  }
  if (run.run_attempt !== 1) {
    return none("it has been run again already");
  }
  if (bad.length === 0) {
    return none("no job failed");
  }
  const ran = bad.filter((job) => !neverRan(job));
  if (ran.length > 0) {
    return none(`${ran.map((job) => job.name).join(", ")} ran and failed`);
  }
  const reason = `${names.join(", ")} never got a runner`;
  if (
    run.event === "pull_request" &&
    head !== undefined &&
    head !== run.head_sha
  ) {
    return none(`${reason}, but a newer commit was pushed since`);
  }
  if (run.event === "pull_request" || run.event === "push") {
    return { action: "rerun", reason, jobs: names };
  }
  const pullRequest = /^gh-readonly-queue\/[^/]+\/pr-(\d+)-/.exec(
    run.head_branch,
  )?.[1];
  if (run.event === "merge_group" && pullRequest !== undefined) {
    return {
      action: "comment",
      reason,
      jobs: names,
      pullRequest: Number(pullRequest),
    };
  }
  return none(`a ${run.event} run is not retried`);
}

const gh = (args: string[]) => {
  const result = spawnSync("gh", args, { encoding: "utf8" });
  if (result.status !== 0) {
    throw new Error(`gh ${args.join(" ")}: ${result.stderr.trim()}`);
  }
  return result.stdout;
};

if (process.argv[1]?.endsWith("ci-retry.ts") === true) {
  const id = process.argv[2] ?? "";
  const repository = process.env["GITHUB_REPOSITORY"] ?? "";
  if (!/^\d+$/.test(id) || repository === "") {
    console.error(
      "Usage: GITHUB_REPOSITORY=owner/repo node scripts/ci-retry.ts <run id>",
    );
    process.exit(2);
  }
  const runs = `repos/${repository}/actions/runs/${id}`;
  const run = JSON.parse(gh(["api", runs])) as Run;
  // Every page: a run has more jobs than the 30 of a page.
  const pages = JSON.parse(
    gh([
      "api",
      "--paginate",
      "--slurp",
      `${runs}/jobs?filter=latest&per_page=100`,
    ]),
  ) as { jobs: Job[] }[];
  const head =
    run.event === "pull_request"
      ? (
          JSON.parse(
            gh(["api", `repos/${repository}/branches/${run.head_branch}`]),
          ) as { commit: { sha: string } }
        ).commit.sha
      : undefined;
  const decision = decide(
    run,
    pages.flatMap((page) => page.jobs),
    head,
  );
  let said = `Not retried: ${decision.reason}.`;
  if (decision.action === "rerun") {
    gh(["api", "-X", "POST", `${runs}/rerun-failed-jobs`]);
    said = `Rerunning the failed jobs once: ${decision.reason}.`;
  } else if (decision.action === "comment") {
    gh([
      "api",
      "-X",
      "POST",
      `repos/${repository}/issues/${String(decision.pullRequest)}/comments`,
      "-f",
      `body=The merge queue's CI run for this pull request (${id}) failed only because ` +
        `${decision.jobs.join(", ")} never got a runner; nothing in the change failed. It can ` +
        "go back in the queue.",
    ]);
    said = `Commented on #${String(decision.pullRequest)}: ${decision.reason}.`;
  }
  console.log(said);
  const summary = process.env["GITHUB_STEP_SUMMARY"];
  if (summary) {
    appendFileSync(summary, `${said}\n`);
  }
}
