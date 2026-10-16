// A release is made only from a commit on main (scripts/release-on-main.ts), checked here in a
// repository made for each test: main with two commits, and a side branch with one of its own.
import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

import { onMain } from "../release-on-main.ts";

function repository() {
  const dir = mkdtempSync(join(tmpdir(), "release-on-main-"));
  const git = (...args: string[]) =>
    execFileSync("git", args, { cwd: dir, encoding: "utf8" }).trim();
  git("init", "-q", "-b", "main");
  git("config", "user.email", "test@example.com");
  git("config", "user.name", "Test");
  git("commit", "-q", "--allow-empty", "-m", "older");
  const older = git("rev-parse", "HEAD");
  git("commit", "-q", "--allow-empty", "-m", "newest");
  const newest = git("rev-parse", "HEAD");
  git("switch", "-q", "-c", "side", older);
  git("commit", "-q", "--allow-empty", "-m", "side");
  const side = git("rev-parse", "HEAD");
  return { dir, older, newest, side };
}

const script = join(import.meta.dirname, "..", "release-on-main.ts");

describe("a release's commit on main", () => {
  test("is main's newest, or an older commit of main", () => {
    const repo = repository();
    expect(onMain(repo.dir, repo.newest, "main")).toBe(true);
    expect(onMain(repo.dir, repo.older, "main")).toBe(true);
  });

  test("is not a commit only on another branch", () => {
    const repo = repository();
    expect(onMain(repo.dir, repo.side, "main")).toBe(false);
  });

  test("cannot be told without main, and then refuses rather than guesses", () => {
    const repo = repository();
    expect(() => onMain(repo.dir, repo.newest, "origin/main")).toThrow(
      /cannot tell/u,
    );
  });

  test.each([
    ["newest", 0, ""],
    ["side", 1, "is not on main"],
  ] as const)(
    "from the command line, as the release runs it: %s exits %d",
    (which, status, message) => {
      const repo = repository();
      const result = spawnSync(
        process.execPath,
        [script, repo[which], "main"],
        { cwd: repo.dir, encoding: "utf8" },
      );
      expect(result.status).toBe(status);
      expect(result.stderr).toContain(message);
    },
  );
});
