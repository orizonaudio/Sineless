import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { changedFiles, changesCode } from "../changed-code.ts";

describe("a change", () => {
  test.each([
    [["HOSTS.md"]],
    [["CONTRIBUTING.md", "README.md", "engine/README.md"]],
    [[".github/PULL_REQUEST_TEMPLATE.md"]],
  ])("to documentation only, %j, skips the builds and tests", (files) => {
    expect(changesCode(files)).toBe(false);
  });

  test.each([
    [["engine/src/Analyzer.cpp"]],
    [["README.md", "ui/src/app.ts"]],
    [["LICENSE"]], // The plugin bundles it.
    [[".github/ISSUE_TEMPLATE/bug.yml"]], // The format check covers it.
    [[".github/workflows/ci.yml"]],
    [["CMakeLists.txt"]],
  ])("that touches code, %j, runs the builds and tests", (files) => {
    expect(changesCode(files)).toBe(true);
  });

  test("whose files could not be listed runs the builds and tests", () => {
    expect(changesCode(undefined)).toBe(true);
  });

  test("that lists no files runs the builds and tests", () => {
    expect(changesCode([])).toBe(true);
  });
});

describe("the files a change touches", () => {
  let repository: string | undefined;
  afterEach(() => {
    if (repository !== undefined) {
      rmSync(repository, { recursive: true, force: true });
    }
  });

  const git = (...args: string[]) =>
    execFileSync("git", args, { cwd: repository, stdio: "ignore" });

  // A repository with one commit holding a.cpp, then a second that renames it to a.md.
  const renamed = () => {
    repository = mkdtempSync(join(tmpdir(), "changed-code-"));
    git("init", "-q");
    git("config", "user.email", "test@example.com");
    git("config", "user.name", "Test");
    writeFileSync(join(repository, "a.cpp"), "int main() { return 0; }\n");
    git("add", "a.cpp");
    git("commit", "-q", "-m", "Add a.cpp");
    git("mv", "a.cpp", "a.md");
    git("commit", "-q", "-m", "Rename a.cpp to a.md");
    return repository;
  };

  test("include both sides of a rename, so a code file renamed to .md is code", () => {
    const files = changedFiles("HEAD~1..HEAD", renamed());
    expect(files).toEqual(expect.arrayContaining(["a.cpp", "a.md"]));
    expect(changesCode(files)).toBe(true);
  });

  test("are not known without a range, as in a run by hand", () => {
    expect(changedFiles("")).toBeUndefined();
  });

  test("are not known for a range git cannot list, as after a new branch's first push", () => {
    expect(
      changedFiles("0000000000000000000000000000000000000000..HEAD", renamed()),
    ).toBeUndefined();
  });
});
