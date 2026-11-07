// Whether the merge queue already tested a commit pushed to main (scripts/queue-tested.ts): if it
// did, CI on the push only fuzzes and fills the caches; when unsure, everything runs.
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

import { queueTested, type Runs } from "../queue-tested.ts";
import { variables } from "./check-harness.ts";

const fixtures = resolve(import.meta.dirname, "fixtures/queue-tested");
const recorded = (name: string) =>
  JSON.parse(readFileSync(join(fixtures, `${name}.json`), "utf8")) as Runs;

// From success.json: the newest successful queue run and its tree.
const sha = "d0dfa4713938da9a55edc8e7e71dfaca2c2bffb2";
const tree = "8419cca6ece0da1e53fa0e9c4be3d009a7431105";

describe("a commit pushed to main", () => {
  test("was tested when a successful queue run had the same commit", () => {
    expect(queueTested(recorded("success"), sha, "another tree")).toBe(true);
  });

  test("was tested when a successful queue run had the same tree under another commit", () => {
    expect(queueTested(recorded("success"), "another commit", tree)).toBe(true);
  });

  test("was not tested when the queue run with the same commit failed", () => {
    const failed = recorded("failure");
    const run = failed.workflow_runs[0];
    expect(
      queueTested(failed, run?.head_sha ?? "", run?.head_commit?.tree_id ?? ""),
    ).toBe(false);
  });

  test("was not tested when no queue run matches", () => {
    expect(
      queueTested(recorded("success"), "another commit", "another tree"),
    ).toBe(false);
  });
});

// The script as CI's changes job runs it, with a fake gh.
describe("the command", () => {
  let folder: string | undefined;
  afterEach(() => {
    if (folder !== undefined) {
      rmSync(folder, { recursive: true, force: true });
      folder = undefined;
    }
  });

  const command = (gh: string) => {
    folder = mkdtempSync(join(tmpdir(), "queue-tested-"));
    mkdirSync(join(folder, "bin"));
    writeFileSync(join(folder, "bin/gh"), `#!/bin/bash\n${gh}\n`);
    chmodSync(join(folder, "bin/gh"), 0o755);
    const result = spawnSync(
      process.execPath,
      [resolve(import.meta.dirname, "../queue-tested.ts"), "HEAD"],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          ...variables(
            ["PATH", `${join(folder, "bin")}:${process.env["PATH"] ?? ""}`],
            ["GITHUB_REPOSITORY", "orizonaudio/Sineless"],
          ),
        },
      },
    );
    return { status: result.status, output: result.stdout.trim() };
  };

  test("prints true when the queue tested the commit", () => {
    const head = spawnSync("git", ["rev-parse", "HEAD"], {
      encoding: "utf8",
    }).stdout.trim();
    // GitHub's answer, in its own field names.
    const runs = `{"workflow_runs": [{"head_sha": "${head}", "conclusion": "success"}]}`;
    expect(command(`echo '${runs}'`)).toEqual({ status: 0, output: "true" });
  });

  test("prints false, and succeeds, when GitHub cannot be asked", () => {
    expect(command("exit 1")).toEqual({ status: 0, output: "false" });
  });

  test("prints false, and succeeds, when GitHub's answer is not a run list", () => {
    expect(command("echo '{}'")).toEqual({ status: 0, output: "false" });
  });
});
