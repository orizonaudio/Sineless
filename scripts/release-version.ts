// The version a release builds: the one in CMakeLists.txt's project(... VERSION x.y.z ...), and
// on a tag, only if the tag is vX.Y.Z naming that same version. Prints it, or the reason there
// is none and exits 1 (.github/workflows/release.yml).
//   node scripts/release-version.ts [tag]
import { readFileSync } from "node:fs";

// The version of `cmakeLists`, checked against `tag` if there is one; an error otherwise.
export function releaseVersion(
  cmakeLists: string,
  tag?: string,
): { version: string } | { error: string } {
  const version = /^project\(\S+ VERSION (\d+\.\d+\.\d+)\b/mu.exec(
    cmakeLists,
  )?.[1];
  if (version === undefined) {
    return { error: "No project(... VERSION x.y.z ...) in CMakeLists.txt" };
  }
  if (tag !== undefined) {
    if (!/^v\d+\.\d+\.\d+$/u.test(tag)) {
      return { error: `The tag ${tag} is not of the form vX.Y.Z` };
    }
    if (tag !== `v${version}`) {
      return {
        error: `The tag ${tag} does not match version ${version} in CMakeLists.txt`,
      };
    }
  }
  return { version };
}

if (process.argv[1]?.endsWith("release-version.ts") === true) {
  const tag = process.argv[2];
  const result = releaseVersion(
    readFileSync("CMakeLists.txt", "utf8"),
    tag === "" ? undefined : tag,
  );
  if ("error" in result) {
    console.error(result.error);
    process.exit(1);
  }
  console.log(result.version);
}
