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

  test.each(["v0.4.3", "v0.4.1", "v1.4.2"])(
    "is not released from %s, a tag naming another version",
    (tag) => {
      expect(releaseVersion(cmakeLists, tag)).toEqual({
        error: `The tag ${tag} does not match version 0.4.2 in CMakeLists.txt`,
      });
    },
  );

  test.each(["0.4.2", "v0.4", "v0.4.2-rc.1", "release-0.4.2"])(
    "is not released from %s, a tag not of the form vX.Y.Z",
    (tag) => {
      expect(releaseVersion(cmakeLists, tag)).toEqual({
        error: `The tag ${tag} is not of the form vX.Y.Z`,
      });
    },
  );

  test("is none without a project version", () => {
    expect(
      releaseVersion("project(OrizonOscilloscope LANGUAGES CXX)\n"),
    ).toEqual({
      error: "No project(... VERSION x.y.z ...) in CMakeLists.txt",
    });
  });

  test("reads the repository's own CMakeLists.txt", () => {
    expect(
      releaseVersion(readFileSync("CMakeLists.txt", "utf8")),
    ).toHaveProperty("version");
  });
});
