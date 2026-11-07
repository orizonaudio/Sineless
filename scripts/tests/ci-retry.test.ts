// A CI run that failed only because jobs never got a runner (scripts/ci-retry.ts): rerun once
// on a pull request or main, and for a merge-queue run, a comment saying so, since a queue entry
// made with the workflow's own token would never start CI. Any job that ran and failed means the
// change, or a flake in it, is the cause, and nothing is retried. Recorded from this repository's
// runs (each job's name, conclusion, runner and steps).
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { decide, type Job, type Run } from "../ci-retry.ts";

// Environment variables, whose upper-case names the naming rule would refuse as object keys.
const variables = (...pairs: [string, string][]) => Object.fromEntries(pairs);

type Recorded = { run: Run; jobs: { jobs: Job[] } };
const recorded = (name: string) =>
  JSON.parse(
    readFileSync(
      resolve(import.meta.dirname, `fixtures/ci-retry/${name}.json`),
      "utf8",
    ),
  ) as Recorded;
const decision = (name: string, run: Partial<Run> = {}) => {
  const { run: recordedRun, jobs } = recorded(name);
  return decide({ ...recordedRun, ...run }, jobs.jobs);
};

describe("a failed run whose only failures never got a runner", () => {
  test("on a pull request is rerun, naming the jobs", () => {
    expect(decision("pull-request-never-ran")).toMatchObject({
      action: "rerun",
      jobs: ["macos-unbranded"],
    });
  });

  test("on main is rerun", () => {
    expect(decision("push-never-ran")).toMatchObject({
      action: "rerun",
      jobs: ["app"],
    });
  });

  test("in the merge queue gets a comment on its pull request, not a rerun", () => {
    expect(decision("merge-queue-never-ran")).toMatchObject({
      action: "comment",
      pullRequest: 324,
      jobs: ["app", "macos", "macos-glue", "macos-unbranded"],
    });
  });

  test("is not retried twice", () => {
    expect(
      decision(
        "pull-request-never-ran",
        Object.fromEntries([["run_attempt", 2]]),
      ).action,
    ).toBe("none");
  });
});

describe("a run is left alone", () => {
  test("when a job beside one that never ran failed in its steps", () => {
    expect(decision("pull-request-mixed").action).toBe("none");
  });

  // A job that got a runner but failed setting itself up did start, on that runner.
  test("when a failed job got a runner, even with no step past its set-up", () => {
    const { run, jobs } = recorded("pull-request-never-ran");
    const setUp = jobs.jobs.map((job) =>
      job.conclusion === "cancelled"
        ? Object.assign(
            {},
            job,
            Object.fromEntries([["runner_name", "GitHub Actions 1"]]),
            {
              conclusion: "failure",
              steps: [{ name: "Set up job", conclusion: "failure" }],
            },
          )
        : job,
    );
    expect(decide(run, setUp).action).toBe("none");
  });

  test("but not for a job with no runner whose set-up alone is recorded", () => {
    const { run, jobs } = recorded("pull-request-never-ran");
    const setUp = jobs.jobs.map((job) =>
      job.conclusion === "cancelled"
        ? { ...job, steps: [{ name: "Set up job", conclusion: "failure" }] }
        : job,
    );
    expect(decide(run, setUp).action).toBe("rerun");
  });

  test("when it did not fail", () => {
    expect(
      decision("pull-request-never-ran", { conclusion: "cancelled" }).action,
    ).toBe("none");
  });

  test("when it was a run by hand", () => {
    expect(
      decision("pull-request-never-ran", { event: "workflow_dispatch" }).action,
    ).toBe("none");
  });
});

// The script as ci-retry.yml runs it, with a fake gh that answers by the request and logs it.
describe("the command", () => {
  let folder: string | undefined;
  afterEach(() => {
    if (folder !== undefined) {
      rmSync(folder, { recursive: true, force: true });
      folder = undefined;
    }
  });

  const command = (run: Run, pages: { jobs: Job[] }[]) => {
    folder = mkdtempSync(join(tmpdir(), "ci-retry-"));
    mkdirSync(join(folder, "bin"));
    writeFileSync(join(folder, "run.json"), JSON.stringify(run));
    writeFileSync(join(folder, "pages.json"), JSON.stringify(pages));
    writeFileSync(
      join(folder, "bin/gh"),
      `#!/bin/bash
folder="$(dirname "$0")/.."
echo "$*" >> "$folder/asked"
case "$*" in
  *"-X POST"*) ;;
  *"/jobs"*) cat "$folder/pages.json" ;;
  *) cat "$folder/run.json" ;;
esac
`,
    );
    chmodSync(join(folder, "bin/gh"), 0o755);
    const result = spawnSync(
      process.execPath,
      [resolve(import.meta.dirname, "../ci-retry.ts"), String(run.id)],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          ...variables(
            ["PATH", `${join(folder, "bin")}:${process.env["PATH"] ?? ""}`],
            ["GITHUB_REPOSITORY", "orizonaudio/Sineless"],
            ["GITHUB_STEP_SUMMARY", join(folder, "summary")],
          ),
        },
      },
    );
    const asked = readFileSync(join(folder, "asked"), "utf8")
      .trim()
      .split("\n");
    return { status: result.status, asked };
  };

  test("reads every page of the jobs and reruns a pull request's failed jobs", () => {
    const { run, jobs } = recorded("pull-request-never-ran");
    const { asked, status } = command(run, [jobs]);
    expect(asked).toContainEqual(
      expect.stringMatching(
        /--paginate --slurp .*\/jobs\?filter=latest&per_page=100/,
      ),
    );
    expect(asked).toContain(
      `api -X POST repos/orizonaudio/Sineless/actions/runs/${String(run.id)}/rerun-failed-jobs`,
    );
    expect(status).toBe(0);
  });

  test("comments on a merge-queue run's pull request", () => {
    const { run, jobs } = recorded("merge-queue-never-ran");
    const { asked } = command(run, [jobs]);
    expect(asked).toContainEqual(
      expect.stringMatching(
        /^api -X POST repos\/orizonaudio\/Sineless\/issues\/324\/comments -f body=.*macos-unbranded/,
      ),
    );
    expect(asked.join("\n")).not.toContain("rerun");
  });

  test("leaves a run alone whose step failure is on the second page", () => {
    const { run, jobs } = recorded("pull-request-mixed");
    const neverRan = jobs.jobs.filter(
      (job) => job.conclusion === "cancelled" && job.steps.length === 0,
    );
    const rest = jobs.jobs.filter((job) => !neverRan.includes(job));
    const { asked } = command(run, [{ jobs: neverRan }, { jobs: rest }]);
    expect(asked.join("\n")).not.toContain("-X POST");
  });
});
