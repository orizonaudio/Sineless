// The sanitizer runs pick their tests by name (CMakePresets.json, each test preset's
// filter.include.name, a regex ctest searches every test's name with). A test added under a name
// no filter matches runs under no sanitizer, and nothing else says so: noTestsAction catches a
// filter that selects nothing, not one that selects too little. These tests read the filters and
// the GoogleTest sources and name every test a filter leaves out that it is there to run.
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

import { describe, expect, test } from "vitest";

const root = join(import.meta.dirname, "..", "..");

interface TestPreset {
  name: string;
  filter?: { include?: { name?: string } };
}

const presets = (
  JSON.parse(readFileSync(join(root, "CMakePresets.json"), "utf8")) as {
    testPresets: TestPreset[];
  }
).testPresets;

// The preset's name filter, as a regular expression. CMake's regex syntax (string(REGEX)) and
// JavaScript's read the constructs the filters use (^, |, groups, \.) the same way.
function filterOf(preset: string): RegExp {
  const pattern = presets.find((p) => p.name === preset)?.filter?.include?.name;
  if (pattern === undefined) {
    throw new Error(`No test preset ${preset} with a name filter`);
  }
  return new RegExp(pattern);
}

interface SourceTest {
  // The names ctest gives the test (gtest_discover_tests with no prefix: GoogleTest's own,
  // Instantiation/Suite.Test for a value-parameterised test, before its parameter's name).
  names: string[];
  startsAThread: boolean;
}

// The GoogleTest tests in `source`. A test's body is its text up to the next test.
function testsIn(source: string): SourceTest[] {
  const instantiations = new Map<string, string[]>();
  for (const match of source.matchAll(
    /INSTANTIATE_TEST_SUITE_P\(\s*(\w+)\s*,\s*(\w+)/g,
  )) {
    const [, prefix = "", suite = ""] = match;
    instantiations.set(suite, [...(instantiations.get(suite) ?? []), prefix]);
  }
  const macros = [
    ...source.matchAll(/^\s*(TEST|TEST_F|TEST_P)\(\s*(\w+)\s*,\s*(\w+)\s*\)/gm),
  ];
  return macros.map((macro, index) => {
    const [, kind, suite = "", name = ""] = macro;
    const body = source.slice(
      macro.index + macro[0].length,
      macros[index + 1]?.index ?? source.length,
    );
    const names =
      kind === "TEST_P"
        ? (instantiations.get(suite) ?? []).map(
            (prefix) => `${prefix}/${suite}.${name}`,
          )
        : [`${suite}.${name}`];
    // Errs towards flagging: a body that only names std::thread::id counts too. Threads a helper
    // outside the test, or the product itself, starts are not seen.
    return { names, startsAThread: /std::j?thread\b/.test(body) };
  });
}

function sources(
  folder: string,
  keep: (path: string) => boolean,
  recursive = false,
): string[] {
  return (
    readdirSync(join(root, folder), { recursive })
      // Paths with "/" on every platform: npm test runs on Windows too.
      .map((entry) => `${folder}/${entry.toString().replaceAll("\\", "/")}`)
      .filter(keep)
  );
}

// Test sources built on every platform, Linux included.
const shared = [
  ...sources(
    "engine/tests",
    (path) => path.endsWith(".cpp") && !path.startsWith("engine/tests/fuzz/"),
    true,
  ),
  ...sources("plugin/tests", (path) => path.endsWith(".cpp")),
  ...sources(
    "editor/tests",
    (path) => path.endsWith(".cpp") && !path.endsWith("Win.cpp"),
  ),
];

// Test sources built on macOS alone: the editor's web view and native drawing, and the Audio Unit.
const macOnly = [
  ...sources("editor/tests", (path) => path.endsWith(".mm")),
  ...sources(
    "au/tests",
    (path) => path.endsWith(".cpp") || path.endsWith(".mm"),
  ),
];

function read(path: string): SourceTest[] {
  return testsIn(readFileSync(join(root, path), "utf8"));
}

describe("the scan of the test sources", () => {
  test.each(macOnly)("finds the tests in %s", (path) => {
    expect(read(path).length).toBeGreaterThan(0);
  });

  test("names every parameterised test, by an instantiation in its file", () => {
    const unnamed = [...shared, ...macOnly].flatMap((path) =>
      read(path)
        .filter((t) => t.names.length === 0)
        .map(() => path),
    );
    expect(unnamed).toEqual([]);
  });
});

// The macOS-only tests the AddressSanitizer run leaves out, and why. The Component suite reads the
// Audio Unit's and the VST3 plugin's bundles (their property lists) and runs none of their code, so
// the sanitizer has nothing to watch; and the asan build preset does not build the VST3 plugin,
// whose bundle one of them reads.
const notUnderAsan = /^Component\./;

describe("the sanitizer runs take the tests they are there for", () => {
  test("every test that starts a thread runs under ThreadSanitizer on Linux (linux-tsan)", () => {
    const threaded = shared.flatMap((path) =>
      read(path)
        .filter((t) => t.startsAThread)
        .flatMap((t) => t.names),
    );
    expect(threaded.length).toBeGreaterThan(0);
    const filter = filterOf("linux-tsan");
    expect(threaded.filter((name) => !filter.test(name))).toEqual([]);
  });

  test("every macOS-only test runs under AddressSanitizer on macOS (asan)", () => {
    const filter = filterOf("asan");
    const left = macOnly.flatMap((path) =>
      read(path)
        .flatMap((t) => t.names)
        .filter((name) => !filter.test(name) && !notUnderAsan.test(name)),
    );
    expect(left).toEqual([]);
  });
});
