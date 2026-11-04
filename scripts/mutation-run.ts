// Runs the C++ mutants Mull built into a test binary, a share at a time, in place of mull-runner:
// mull-runner has no way to split its mutants between jobs, writes nothing until every mutant has
// run, and limits each mutant only to ten times the tests' own time. Here each mutant runs as Mull
// runs it (the test binary with an environment variable named after the mutant set to 1), its
// result is written as soon as it is known, a mutant that takes longer than the limit is stopped
// and counted as timed out, and every result is written within the budget: the seconds from the
// run's start, the tests' run with no mutant included, by which it must end (CI's share job is
// cancelled soon after, losing whatever is still running). No mutant starts once the budget is
// spent, and one still running when it ends is stopped and reported as not run, as what it would
// show is unknown; the mutants left are reported as not run too. Before the mutants, each test
// binary (GoogleTest) runs once with no mutant, spread over the workers as GoogleTest shards: it
// must pass, and each test's time is kept.
// After the first binary, each further one (after another --) runs a mutant the ones before it
// did not notice: the Audio Unit's view tests notice some of its mutants that its other tests
// miss (scripts/mutation-au.sh). --workers sets how many mutants run at once (default: one a core).
//   node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl>
//       --limit <seconds a binary> --budget <seconds in all> [--workers <n>]
//       -- <test binary> [arguments] [-- <test binary> [arguments]]...
//   node scripts/mutation-run.ts merge <dry-run.json> <results.jsonl>...   (prints the report)
//   node scripts/mutation-run.ts combine <dry-run.json>...   (prints the lists as one)
// The dry run is Mull's report of every mutant with no test run (mull-runner --dry-run). A share
// with no mutant runs no test: a module with fewer mutants than shares leaves some empty
// (scripts/mutation-cpp.sh runs each module's share against its own tests).
import { spawn } from "node:child_process";
import {
  appendFileSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { availableParallelism, tmpdir } from "node:os";
import { join } from "node:path";

import type { Report } from "./mutation-summary.ts";

// A mutant as Mull's report lists it.
interface ReportMutant {
  id: string;
  mutatorName: string;
  replacement?: string;
  location: { start: { line: number; column: number } };
  status: string;
}

export interface Result {
  id: string;
  status: "Killed" | "Survived" | "Timeout" | "NotRun";
}

// Every mutant of the dry run, by id, in a stable order.
export function mutantIds(report: Report): string[] {
  return Object.values(report.files)
    .flatMap(({ mutants }) =>
      (mutants as ReportMutant[]).map((mutant) => mutant.id),
    )
    .sort();
}

// The dry runs of several test binaries as one list, as each lists its own module's files
// (scripts/mutation-cpp.sh): the list CI counts and the shares' results are merged into.
export function combine(reports: Report[]): Report {
  return {
    ...reports[0],
    files: Object.fromEntries(
      reports.flatMap((report) => Object.entries(report.files)),
    ),
  };
}

// Share `share` of `shares` (from 1): every shares-th mutant, so that each share gets some of
// every file rather than whole files, and a large file is shared out too.
export function shareOf(
  ids: string[],
  share: number,
  shares: number,
): string[] {
  if (
    !Number.isInteger(share) ||
    !Number.isInteger(shares) ||
    share < 1 ||
    share > shares
  ) {
    throw new Error(`No share ${String(share)} of ${String(shares)}`);
  }
  return ids.filter((_, index) => index % shares === share - 1);
}

// The report with each mutant's status from the results; a mutant no result names was not run.
// Pending is the report format's status for a mutant not run: it counts neither as noticed nor
// as not (scripts/mutation-summary.ts).
export function merge(report: Report, results: Result[]): Report {
  const statuses = new Map(results.map((result) => [result.id, result.status]));
  return {
    ...report,
    files: Object.fromEntries(
      Object.entries(report.files).map(([file, { mutants }]) => [
        file,
        {
          ...report.files[file],
          mutants: (mutants as ReportMutant[]).map((mutant) => {
            const status = statuses.get(mutant.id);
            return {
              ...mutant,
              status:
                status === undefined || status === "NotRun"
                  ? "Pending"
                  : status,
            };
          }),
        },
      ]),
    ),
  };
}

// The mutants whose switch the test binary does not hold. Mull's code in the binary reads each
// mutant's switch from the environment variable named after its id, so each id is in the binary
// as text; one missing means this Mull switches mutants on some other way, and every mutant run
// here would go unnoticed for want of being switched on.
export function missingSwitches(binary: Buffer, ids: string[]): string[] {
  return ids.filter((id) => !binary.includes(`${id}\0`));
}

// How a test run with a mutant ends: the tests failing notice it, passing do not, and one stopped
// at the limit has hung a test, which also notices it.
export function statusOf(
  exitCode: number | null,
  timedOut: boolean,
): Result["status"] {
  if (timedOut) {
    return "Timeout";
  }
  return exitCode === 0 ? "Survived" : "Killed";
}

// A test binary and its arguments.
export interface TestCommand {
  command: string;
  args: string[];
}

interface RunOptions {
  ids: string[];
  results: string;
  limitSeconds: number;
  // When the budget ends, in milliseconds since the epoch (Date.now()).
  endsAt: number;
  tests: TestCommand[];
  workers?: number | undefined;
}

type TestOptions = Pick<RunOptions, "limitSeconds" | "endsAt" | "tests">;

// The tests run with the mutant `id` switched on, or with none: each binary in turn, until one
// fails or is stopped at the limit, which notices the mutant.
async function runTests(
  options: TestOptions,
  id?: string,
): Promise<Result["status"]> {
  for (const test of options.tests) {
    const status = await runBinary(test, options, id);
    if (status !== "Survived") {
      return status;
    }
  }
  return "Survived";
}

// One binary's run, stopped at its limit or at the end of the budget, whichever comes first. One
// the budget stops before its own limit is not run: its hang, if it is one, is not shown.
function runBinary(
  test: TestCommand,
  options: Pick<TestOptions, "limitSeconds" | "endsAt">,
  id?: string,
): Promise<Result["status"]> {
  const limit = options.limitSeconds * 1000;
  const left = options.endsAt - Date.now();
  if (left <= 0) {
    return Promise.resolve("NotRun");
  }
  const capped = left < limit;
  return new Promise((resolve) => {
    const child = spawn(test.command, test.args, {
      env: id === undefined ? process.env : { ...process.env, [id]: "1" },
      stdio: "ignore",
    });
    let timedOut = false;
    const timer = setTimeout(
      () => {
        timedOut = true;
        child.kill("SIGKILL");
      },
      Math.min(limit, left),
    );
    child.on("error", () => {
      clearTimeout(timer);
      resolve("Killed");
    });
    child.on("exit", (code) => {
      clearTimeout(timer);
      resolve(timedOut && capped ? "NotRun" : statusOf(code, timedOut));
    });
  });
}

// The tests a GoogleTest binary's --gtest_list_tests prints, each as Suite.Name: a suite on a line
// of its own ending in a dot, its tests indented under it, a parameterised one followed by its
// "# GetParam() = ..." comment, which is not part of its name. Other lines (gtest_main's "Running
// main() from ...") are not tests.
export function listedTests(output: string): string[] {
  const names: string[] = [];
  let suite: string | undefined;
  for (const line of output.split("\n")) {
    const suiteLine = /^(\S+)\.(?:\s+#.*)?$/u.exec(line);
    const testLine = /^ {2}(\S+)/u.exec(line);
    if (suiteLine !== null) {
      suite = suiteLine[1];
    } else if (testLine !== null && suite !== undefined) {
      names.push(`${suite}.${testLine[1] ?? ""}`);
    }
  }
  return names;
}

// Each test's time from a GoogleTest JSON report (--gtest_output=json), in seconds, by the name
// --gtest_list_tests prints: each suite is an object with its name and a "testsuite" array of its
// tests, each with its own name and its time as "0.016s" (GoogleTest's gtest.cc,
// OutputJsonTestInfo and PrintJsonTestSuite).
export function reportedTimes(json: string): Map<string, number> {
  const report = JSON.parse(json) as {
    testsuites?: {
      name: string;
      testsuite?: { name: string; time?: string }[];
    }[];
  };
  const times = new Map<string, number>();
  for (const suite of report.testsuites ?? []) {
    for (const test of suite.testsuite ?? []) {
      times.set(
        `${suite.name}.${test.name}`,
        Number.parseFloat(test.time ?? "0"),
      );
    }
  }
  return times;
}

// A test binary's run with no mutant: how long it takes to start and each test's time, by name.
export interface Baseline {
  startupSeconds: number;
  testSeconds: Map<string, number>;
}

// The time a run of every test in the binary takes, start-up included.
export function wholeSeconds(baseline: Baseline): number {
  let total = baseline.startupSeconds;
  for (const seconds of baseline.testSeconds.values()) {
    total += seconds;
  }
  return total;
}

interface Captured {
  code: number | null;
  timedOut: boolean;
  stdout: string;
  seconds: number;
}

// A run of a binary with its output kept, stopped after `limitMs`.
function capture(
  test: TestCommand,
  args: string[],
  env: NodeJS.ProcessEnv,
  limitMs: number,
): Promise<Captured> {
  const started = Date.now();
  return new Promise((resolve) => {
    const child = spawn(test.command, [...test.args, ...args], {
      env,
      stdio: ["ignore", "pipe", "ignore"],
    });
    let stdout = "";
    child.stdout.on("data", (chunk: Buffer) => {
      stdout += chunk.toString();
    });
    let timedOut = false;
    const timer = setTimeout(
      () => {
        timedOut = true;
        child.kill("SIGKILL");
      },
      Math.max(0, limitMs),
    );
    const end = (code: number | null) => {
      clearTimeout(timer);
      resolve({
        code,
        timedOut,
        stdout,
        seconds: (Date.now() - started) / 1000,
      });
    };
    child.on("error", () => {
      end(null);
    });
    child.on("close", end);
  });
}

// The tests' run with no mutant, each binary spread over `workers` GoogleTest shards at once
// (GTEST_TOTAL_SHARDS and GTEST_SHARD_INDEX, each test in one shard), as the mutants will load the
// runner: each test's time, and the binary's start-up (a shard's time less its tests', the largest
// of the shards). Fails, naming why, when a test fails with no mutant (every mutant would then
// read as noticed), when a run outlasts the limit or the budget, or when a test the binary lists
// has no time in its report (every limit taken from those times would be wrong).
export async function baseline(
  options: TestOptions & { workers?: number | undefined },
): Promise<Baseline[] | string> {
  const shards = options.workers ?? availableParallelism();
  const baselines: Baseline[] = [];
  for (const test of options.tests) {
    const name = test.command;
    const cap = () =>
      Math.min(options.limitSeconds * 1000, options.endsAt - Date.now());
    const list = await capture(
      test,
      ["--gtest_list_tests"],
      process.env,
      cap(),
    );
    if (list.code !== 0) {
      return `${name} cannot list its tests.`;
    }
    const listed = listedTests(list.stdout);
    const dir = mkdtempSync(join(tmpdir(), "mutation-baseline-"));
    const runs = await Promise.all(
      Array.from({ length: shards }, (_, shard) =>
        capture(
          test,
          [`--gtest_output=json:${join(dir, `${String(shard)}.json`)}`],
          {
            ...process.env,
            // eslint-disable-next-line @typescript-eslint/naming-convention -- GoogleTest names it.
            GTEST_TOTAL_SHARDS: String(shards),
            // eslint-disable-next-line @typescript-eslint/naming-convention -- GoogleTest names it.
            GTEST_SHARD_INDEX: String(shard),
          },
          cap(),
        ),
      ),
    );
    if (runs.some((run) => run.timedOut)) {
      return `${name} takes more than ${String(options.limitSeconds)} s or the budget with no mutant.`;
    }
    if (runs.some((run) => run.code !== 0)) {
      return `${name} fails with no mutant.`;
    }
    const testSeconds = new Map<string, number>();
    let startupSeconds = 0;
    for (const [shard, run] of runs.entries()) {
      const report = join(dir, `${String(shard)}.json`);
      // A shard holding no test still writes a report; one that wrote none ran no test.
      const times = existsSync(report)
        ? reportedTimes(readFileSync(report, "utf8"))
        : new Map<string, number>();
      let tests = 0;
      for (const [testName, seconds] of times) {
        testSeconds.set(testName, seconds);
        tests += seconds;
      }
      startupSeconds = Math.max(startupSeconds, run.seconds - tests);
    }
    const untimed = listed.filter((testName) => !testSeconds.has(testName));
    if (untimed.length > 0) {
      return `${name} lists ${String(untimed.length)} tests its reports give no time for, as ${untimed[0] ?? ""}.`;
    }
    const result = { startupSeconds, testSeconds };
    if (wholeSeconds(result) > options.limitSeconds) {
      return `${name} would take ${wholeSeconds(result).toFixed(0)} s with no mutant, more than the limit of ${String(options.limitSeconds)} s.`;
    }
    baselines.push(result);
  }
  return baselines;
}

async function runOne(options: RunOptions, id: string): Promise<Result> {
  return { id, status: await runTests(options, id) };
}

// Runs the mutants on every core, appending each result as it is known. Once the budget is spent
// no new mutant starts, and those left are written as not run.
export async function run(options: RunOptions): Promise<Result[]> {
  const queue = [...options.ids];
  const done: Result[] = [];
  const record = (result: Result) => {
    done.push(result);
    appendFileSync(options.results, `${JSON.stringify(result)}\n`);
  };
  const worker = async () => {
    for (let id = queue.shift(); id !== undefined; id = queue.shift()) {
      record(
        Date.now() < options.endsAt
          ? await runOne(options, id)
          : { id, status: "NotRun" },
      );
    }
  };
  await Promise.all(
    Array.from({ length: options.workers ?? availableParallelism() }, () =>
      worker(),
    ),
  );
  return done;
}

// The test binaries after the first --, each with its arguments, split at each further --.
export function testCommands(args: string[]): TestCommand[] {
  const groups: string[][] = [[]];
  for (const arg of args.slice(args.indexOf("--") + 1)) {
    if (arg === "--") {
      groups.push([]);
    } else {
      groups.at(-1)?.push(arg);
    }
  }
  return groups.map(([command = "", ...rest]) => ({ command, args: rest }));
}

function readResults(path: string): Result[] {
  if (!existsSync(path)) {
    return [];
  }
  return readFileSync(path, "utf8")
    .split("\n")
    .filter((line) => line.trim() !== "")
    .map((line) => JSON.parse(line) as Result);
}

if (process.argv[1]?.endsWith("mutation-run.ts") === true) {
  const args = process.argv.slice(2);
  // The options come before the test binaries, whose own arguments are not read as options.
  const options = args.includes("--")
    ? args.slice(0, args.indexOf("--"))
    : args;
  const option = (name: string) => {
    const at = options.indexOf(name);
    return at === -1 ? undefined : options[at + 1];
  };
  const [command, dryRun] = args;
  const report =
    dryRun === undefined
      ? undefined
      : (JSON.parse(readFileSync(dryRun, "utf8")) as Report);
  if (command === "run" && report !== undefined) {
    // The budget counts from here: the tests' run with no mutant is within it.
    const started = Date.now();
    const [share, shares] = (args[2] ?? "").split("/").map(Number);
    const results = args[3];
    const tests = testCommands(args);
    const limit = Number(option("--limit"));
    const budget = Number(option("--budget"));
    const workers =
      option("--workers") === undefined
        ? undefined
        : Number(option("--workers"));
    if (
      results === undefined ||
      !args.includes("--") ||
      tests.some((test) => test.command === "") ||
      !(limit > 0) ||
      !(budget > 0) ||
      (workers !== undefined && !(Number.isInteger(workers) && workers > 0))
    ) {
      console.error(
        "run needs <share>/<shares> <results.jsonl> --limit --budget [--workers] -- <binary> [-- <binary>]...",
      );
      process.exit(2);
    }
    // The first binary holds every mutant's switch. The later ones may not: the component's tests
    // load the component built with the mutants rather than link them.
    const binary = tests[0]?.command ?? "";
    const ids = shareOf(mutantIds(report), share ?? 0, shares ?? 0);
    if (ids.length === 0) {
      writeFileSync(results, "");
      console.log("0 mutants run, 0 not run.");
      process.exit(0);
    }
    const missing = missingSwitches(readFileSync(binary), ids);
    if (missing.length > 0) {
      console.error(
        `${binary} has no switch for ${String(missing.length)} of the ${String(ids.length)} mutants, as ${missing[0] ?? ""}: Mull no longer switches them on by environment variable, so none would be tested.`,
      );
      process.exit(1);
    }
    const endsAt = started + budget * 1000;
    const baselines = await baseline({
      limitSeconds: limit,
      endsAt,
      tests,
      workers,
    });
    if (typeof baselines === "string") {
      console.error(
        `${baselines} The tests must pass with no mutant, within ${String(limit)} s and the budget's ${String(budget)} s, or every mutant would read as noticed.`,
      );
      process.exit(1);
    }
    for (const [index, times] of baselines.entries()) {
      console.log(
        `${tests[index]?.command ?? ""}: ${String(times.testSeconds.size)} tests, ${wholeSeconds(times).toFixed(1)} s with no mutant (start-up ${times.startupSeconds.toFixed(1)} s).`,
      );
    }
    writeFileSync(results, "");
    const done = await run({
      ids,
      results,
      limitSeconds: limit,
      endsAt,
      tests,
      workers,
    });
    const notRun = done.filter((result) => result.status === "NotRun").length;
    console.log(
      `${String(done.length - notRun)} mutants run, ${String(notRun)} not run.`,
    );
  } else if (command === "combine") {
    const reports = args
      .slice(1)
      .map((path) => JSON.parse(readFileSync(path, "utf8")) as Report);
    console.log(JSON.stringify(combine(reports)));
  } else if (command === "merge" && report !== undefined) {
    const results = args.slice(2).flatMap(readResults);
    console.log(JSON.stringify(merge(report, results)));
  } else {
    console.error(
      "Usage: node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl> --limit <s> --budget <s> [--workers <n>] -- <binary> [args] [-- <binary> [args]]... | merge <dry-run.json> <results.jsonl>... | combine <dry-run.json>...",
    );
    process.exit(2);
  }
}
