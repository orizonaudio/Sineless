// Whether a change touches more than documentation, which decides whether CI builds and tests it
// (.github/workflows/ci.yml), or with a folder, whether it touches a file in that folder, which
// decides whether a job for that folder alone runs (the Audio Unit's mutation testing, au/src).
// Prints "true" or "false".
//   node scripts/changed-code.ts <git range> [folder]
import { execFileSync } from "node:child_process";

// Documentation: Markdown, which none of the build, the page, the tests or the format check reads
// (.prettierignore). Not the licence, which the plugin bundles, nor YAML such as the issue
// templates, which the format check covers.
export function isDocumentation(path: string): boolean {
  return path.endsWith(".md");
}

// Whether `files`, the paths a change touches, call for the builds and tests: unless every one is
// documentation. A change whose files could not be listed (undefined), or that lists none, counts
// as code, so that a failure here never skips a check.
export function changesCode(files: readonly string[] | undefined): boolean {
  if (files === undefined || files.length === 0) {
    return true;
  }
  return !files.every(isDocumentation);
}

// Whether `files` include one in `folder`. Unknown files, or none listed, count as doing so, as in
// changesCode.
export function changesUnder(
  files: readonly string[] | undefined,
  folder: string,
): boolean {
  if (files === undefined || files.length === 0) {
    return true;
  }
  return files.some((file) => file.startsWith(`${folder}/`));
}

// The paths changed in `range`, both sides of a rename included (a code file renamed to a .md
// name is a code change); undefined when there is no range or git cannot list it.
export function changedFiles(
  range: string,
  cwd = process.cwd(),
): string[] | undefined {
  if (range === "") {
    return undefined;
  }
  try {
    return execFileSync(
      "git",
      ["diff", "--name-only", "--no-renames", range, "--"],
      { cwd, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] },
    )
      .split("\n")
      .filter((line) => line !== "");
  } catch {
    return undefined;
  }
}

if (process.argv[1]?.endsWith("changed-code.ts") === true) {
  const files = changedFiles(process.argv[2] ?? "");
  const folder = process.argv[3];
  console.log(
    String(
      folder === undefined ? changesCode(files) : changesUnder(files, folder),
    ),
  );
}
