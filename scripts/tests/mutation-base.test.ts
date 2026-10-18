// The base Mull compares the working tree with on a pull request: files added since the real base
// must count as changed on every line, or a new source file gets no mutant at all.
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { mullBase } from "../mutation-base.ts";

let repository: string | undefined;
afterEach(() => {
  if (repository !== undefined) {
    rmSync(repository, { recursive: true, force: true });
  }
});

const git = (...args: string[]) =>
  execFileSync("git", args, { cwd: repository, encoding: "utf8" }).trim();

const write = (path: string, text: string) => {
  mkdirSync(join(repository ?? "", path, ".."), { recursive: true });
  writeFileSync(join(repository ?? "", path), text);
};

// A repository whose first commit, the base, holds au/src/Old.cpp; the second changes it, adds
// au/src/New.mm and renames engine/src/A.cpp to engine/src/B.cpp.
const pullRequest = () => {
  repository = mkdtempSync(join(tmpdir(), "mutation-base-"));
  git("init", "-q");
  git("config", "user.email", "test@example.com");
  git("config", "user.name", "Test");
  write("au/src/Old.cpp", "int a = 0;\n");
  write("engine/src/A.cpp", "int b = 0;\n");
  git("add", ".");
  git("commit", "-q", "-m", "Base");
  const base = git("rev-parse", "HEAD");
  write("au/src/Old.cpp", "int a = 1;\n");
  write("au/src/New.mm", "int c = 0;\nint d = 0;\n");
  git("mv", "engine/src/A.cpp", "engine/src/B.cpp");
  git("add", ".");
  git("commit", "-q", "-m", "Change");
  return base;
};

// A file as the tree of `commit` holds it, or undefined where it holds none. Mull's diff reads the
// tree and the working tree only, so a file is a changed one to it only if the tree holds it.
const inTree = (commit: string, path: string) => {
  try {
    return execFileSync("git", ["show", `${commit}:${path}`], {
      cwd: repository,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
    });
  } catch {
    return undefined;
  }
};

describe("the base Mull compares the changed lines with", () => {
  test("holds each file added since the real base, empty, so every line of a new file is a changed line", () => {
    const base = pullRequest();
    expect(inTree(base, "au/src/New.mm")).toBeUndefined();
    expect(
      inTree(mullBase(base, ["au/src"], repository), "au/src/New.mm"),
    ).toBe("");
  });

  test("holds the files the real base had as it had them, so their changed lines stay the same", () => {
    const base = pullRequest();
    const given = mullBase(base, ["au/src"], repository);
    expect(inTree(given, "au/src/Old.cpp")).toBe("int a = 0;\n");
    expect(inTree(given, "engine/src/A.cpp")).toBe("int b = 0;\n");
  });

  test("counts a file renamed since the real base as added, as the new name is new to it", () => {
    const base = pullRequest();
    expect(
      inTree(mullBase(base, ["engine/src"], repository), "engine/src/B.cpp"),
    ).toBe("");
  });

  test("adds only files in the folders mutated", () => {
    const base = pullRequest();
    expect(
      inTree(mullBase(base, ["engine/src"], repository), "au/src/New.mm"),
    ).toBeUndefined();
  });

  test("is the real base when no file was added, and leaves the repository's branches, index and files as they were", () => {
    const base = pullRequest();
    const head = git("rev-parse", "HEAD");
    const status = git("status", "--porcelain");
    const branches = git("branch", "--list");
    expect(mullBase(base, ["protocol/src"], repository)).toBe(base);
    mullBase(base, ["au/src"], repository);
    expect(git("rev-parse", "HEAD")).toBe(head);
    expect(git("status", "--porcelain")).toBe(status);
    expect(git("branch", "--list")).toBe(branches);
    expect(readFileSync(join(repository ?? "", "au/src/New.mm"), "utf8")).toBe(
      "int c = 0;\nint d = 0;\n",
    );
  });

  test("is made where git knows no author, as on a CI runner", () => {
    const base = pullRequest();
    git("config", "--unset", "user.email");
    git("config", "--unset", "user.name");
    const given = execFileSync(
      process.execPath,
      [join(import.meta.dirname, "..", "mutation-base.ts"), base, "au/src"],
      {
        cwd: repository,
        encoding: "utf8",
        // No user, global or system settings, and no author guessed from the machine's names:
        // git knows no author.
        env: {
          ...process.env,
          ...Object.fromEntries([
            ["GIT_CONFIG_GLOBAL", "/dev/null"],
            ["GIT_CONFIG_NOSYSTEM", "1"],
            ["GIT_CONFIG_COUNT", "1"],
            ["GIT_CONFIG_KEY_0", "user.useConfigOnly"],
            ["GIT_CONFIG_VALUE_0", "true"],
            ["HOME", repository ?? ""],
          ]),
        },
      },
    ).trim();
    expect(inTree(given, "au/src/New.mm")).toBe("");
  });

  test("is refused without a folder, rather than adding every file in the repository", () => {
    const base = pullRequest();
    expect(() =>
      execFileSync(
        process.execPath,
        [join(import.meta.dirname, "..", "mutation-base.ts"), base],
        { cwd: repository, stdio: "pipe" },
      ),
    ).toThrow(/Usage: node scripts\/mutation-base.ts <base commit> <folder>/u);
  });
});
