import { readFileSync } from "node:fs";
import { describe, expect, test } from "vitest";

import { releaseVersion } from "../release-version.ts";

const cmakeLists =
  "cmake_minimum_required(VERSION 3.25)\n\nproject(OrizonOscilloscope VERSION 0.4.2 LANGUAGES CXX)\n";

describe("the release version", () => {
  test("is the project's, from a dry run with no tag", () => {
    expect(releaseVersion(cmakeLists)).toEqual({ version: "0.4.2" });
  });

  test("is released from the tag that names it", () => {
    expect(releaseVersion(cmakeLists, "v0.4.2")).toEqual({ version: "0.4.2" });
  });

  // Refused, with an error naming what to fix: the tag, and the version it should name.
  test.each(["v0.4.3", "v0.4.1", "v1.4.2"])(
    "is not released from %s, a tag naming another version",
    (tag) => {
      const result = releaseVersion(cmakeLists, tag);
      expect(result).not.toHaveProperty("version");
      expect(result).toHaveProperty("error", expect.stringContaining(tag));
      expect(result).toHaveProperty("error", expect.stringContaining("0.4.2"));
    },
  );

  test.each(["0.4.2", "v0.4", "v0.4.2-rc.1", "release-0.4.2"])(
    "is not released from %s, a tag not of the form vX.Y.Z",
    (tag) => {
      const result = releaseVersion(cmakeLists, tag);
      expect(result).not.toHaveProperty("version");
      expect(result).toHaveProperty("error", expect.stringContaining(tag));
    },
  );

  test("is none without a project version", () => {
    const result = releaseVersion(
      "project(OrizonOscilloscope LANGUAGES CXX)\n",
    );
    expect(result).not.toHaveProperty("version");
    expect(result).toHaveProperty("error");
  });

  test("reads the repository's own CMakeLists.txt", () => {
    expect(
      releaseVersion(readFileSync("CMakeLists.txt", "utf8")),
    ).toHaveProperty("version");
  });
});
