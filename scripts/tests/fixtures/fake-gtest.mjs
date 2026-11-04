/* global console, process, setInterval, setTimeout */
// Stands in for a GoogleTest binary built with mutants, from the spec in the JSON file named by its
// first argument:
//   { "startupMs": 20, "suites": [{ "name": "Rates/Sweep", "tests": [{ "name": "Draws/0",
//     "param": "44k1", "ms": 30 }] }], "omitFromJson": ["Rates/Sweep.Draws/0"],
//     "failWithNoMutant": false, "lock": "/tmp/lock" }
// --gtest_list_tests prints the tests as GoogleTest does, gtest_main's first line included. A run
// takes its start-up and each test's time, honours GTEST_TOTAL_SHARDS and GTEST_SHARD_INDEX (each
// test in one shard, by its index) and writes --gtest_output=json:<file> in GoogleTest's shape. The
// mutant whose variable is set decides how the run ends, as fake-tests.mjs: "kill:" fails,
// "survive:" passes, "hang:" never ends; each further argument names a prefix this binary also
// fails on. With "lock", a run holds that file while it runs and fails if another run holds it, as
// the Audio Unit's view tests share the window server.
import { readFileSync, rmSync, writeFileSync } from "node:fs";

const [specPath = "", ...extra] = process.argv
  .slice(2)
  .filter((arg) => !arg.startsWith("--gtest"));
const spec = JSON.parse(readFileSync(specPath, "utf8"));
const option = (name) =>
  process.argv
    .find((arg) => arg.startsWith(`${name}=`))
    ?.slice(name.length + 1);

if (process.argv.includes("--gtest_list_tests")) {
  const lines = ["Running main() from /src/googletest/src/gtest_main.cc"];
  for (const suite of spec.suites) {
    lines.push(`${suite.name}.`);
    for (const test of suite.tests) {
      lines.push(
        test.param === undefined
          ? `  ${test.name}`
          : `  ${test.name}  # GetParam() = ${test.param}`,
      );
    }
  }
  console.log(lines.join("\n"));
  process.exit(0);
}

const failsOn = ["kill", ...extra];
const prefixes = [...failsOn, "survive", "hang"].map((name) => `${name}:`);
const id = Object.keys(process.env).find((key) =>
  prefixes.some((prefix) => key.startsWith(prefix)),
);
if (id?.startsWith("hang:")) {
  setInterval(() => undefined, 1000);
} else {
  if (spec.lock !== undefined) {
    try {
      writeFileSync(spec.lock, "", { flag: "wx" });
    } catch {
      process.exit(1);
    }
  }
  const shards = Number(process.env.GTEST_TOTAL_SHARDS ?? "1");
  const shard = Number(process.env.GTEST_SHARD_INDEX ?? "0");
  let index = 0;
  const suites = spec.suites.map((suite) => ({
    name: suite.name,
    tests: suite.tests.filter(() => index++ % shards === shard),
  }));
  const ran = suites.flatMap((suite) => suite.tests);
  const total =
    (spec.startupMs ?? 0) + ran.reduce((sum, test) => sum + test.ms, 0);
  setTimeout(() => {
    const output = option("--gtest_output");
    if (output?.startsWith("json:") === true) {
      const omitted = new Set(spec.omitFromJson ?? []);
      writeFileSync(
        output.slice("json:".length),
        JSON.stringify({
          tests: ran.length,
          testsuites: suites
            .filter((suite) => suite.tests.length > 0)
            .map((suite) => ({
              name: suite.name,
              tests: suite.tests.length,
              testsuite: suite.tests
                .filter((test) => !omitted.has(`${suite.name}.${test.name}`))
                .map((test) => ({
                  name: test.name,
                  ...(test.param === undefined
                    ? {}
                    : { value_param: test.param }),
                  status: "RUN",
                  result: "COMPLETED",
                  time: `${String(test.ms / 1000)}s`,
                  classname: suite.name,
                })),
            })),
        }),
      );
    }
    if (spec.lock !== undefined) {
      rmSync(spec.lock);
    }
    const fails =
      id === undefined
        ? spec.failWithNoMutant === true
        : failsOn.some((name) => id.startsWith(`${name}:`));
    process.exit(fails ? 1 : 0);
  }, total);
}
