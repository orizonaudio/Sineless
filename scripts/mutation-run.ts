// Runs the C++ mutants Mull built into a test binary, a share at a time, in place of mull-runner:
// mull-runner has no way to split its mutants between jobs, writes nothing until every mutant has
// run, and limits each mutant only to ten times the tests' own time. Here each mutant runs as Mull
// runs it (the test binary with an environment variable named after the mutant set to 1), its
// result is written as soon as it is known, a mutant that takes longer than the limit is stopped
// and counted as timed out, and the mutants not reached by the deadline are reported as not run.
// After the first binary, each further one (after another --) runs a mutant the ones before it
// did not notice: the Audio Unit's view tests notice some of its mutants that its other tests
// miss (scripts/mutation-au.sh). --workers sets how many mutants run at once (default: one a core).
//   node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl>
//       --limit <seconds a binary> --deadline <seconds in all> [--workers <n>]
//       -- <test binary> [arguments] [-- <test binary> [arguments]]...
//   node scripts/mutation-run.ts merge <dry-run.json> <results.jsonl>...   (prints the report)
// The dry run is Mull's report of every mutant with no test run (mull-runner --dry-run).
import { spawn } from "node:child_process";
import {
  appendFileSync,
  existsSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { availableParallelism } from "node:os";

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
  deadlineSeconds: number;
  tests: TestCommand[];
  workers?: number | undefined;
}

type TestOptions = Pick<RunOptions, "limitSeconds" | "tests">;

// The tests run with the mutant `id` switched on, or with none: each binary in turn, until one
// fails or is stopped at the limit, which notices the mutant.
async function runTests(
  options: TestOptions,
  id?: string,
): Promise<Result["status"]> {
  for (const test of options.tests) {
    const status = await runBinary(test, options.limitSeconds, id);
    if (status !== "Survived") {
      return status;
    }
  }
  return "Survived";
}

function runBinary(
  test: TestCommand,
  limitSeconds: number,
  id?: string,
): Promise<Result["status"]> {
  return new Promise((resolve) => {
    const child = spawn(test.command, test.args, {
      env: id === undefined ? process.env : { ...process.env, [id]: "1" },
      stdio: "ignore",
    });
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, limitSeconds * 1000);
    child.on("error", () => {
      clearTimeout(timer);
      resolve("Killed");
    });
    child.on("exit", (code) => {
      clearTimeout(timer);
      resolve(statusOf(code, timedOut));
    });
  });
}

// Whether the tests pass with no mutant, within the limit. Where they do not (the binary cannot
// find its data on this machine, say), every mutant would read as noticed.
export async function testsPass(options: TestOptions): Promise<boolean> {
  return (await runTests(options)) === "Survived";
}

async function runOne(options: RunOptions, id: string): Promise<Result> {
  return { id, status: await runTests(options, id) };
}

// Runs the mutants on every core, appending each result as it is known. Once the deadline has
// passed no new mutant starts, and those left are written as not run.
export async function run(options: RunOptions): Promise<Result[]> {
  const deadline = Date.now() + options.deadlineSeconds * 1000;
  const queue = [...options.ids];
  const done: Result[] = [];
  const record = (result: Result) => {
    done.push(result);
    appendFileSync(options.results, `${JSON.stringify(result)}\n`);
  };
  const worker = async () => {
    for (let id = queue.shift(); id !== undefined; id = queue.shift()) {
      record(
        Date.now() < deadline
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
    const [share, shares] = (args[2] ?? "").split("/").map(Number);
    const results = args[3];
    const tests = testCommands(args);
    const limit = Number(option("--limit"));
    const deadline = Number(option("--deadline"));
    const workers =
      option("--workers") === undefined
        ? undefined
        : Number(option("--workers"));
    if (
      results === undefined ||
      !args.includes("--") ||
      tests.some((test) => test.command === "") ||
      !(limit > 0) ||
      !(deadline > 0) ||
      (workers !== undefined && !(Number.isInteger(workers) && workers > 0))
    ) {
      console.error(
        "run needs <share>/<shares> <results.jsonl> --limit --deadline [--workers] -- <binary> [-- <binary>]...",
      );
      process.exit(2);
    }
    // The first binary holds every mutant's switch. The later ones may not: the component's tests
    // load the component built with the mutants rather than link them.
    const binary = tests[0]?.command ?? "";
    const ids = shareOf(mutantIds(report), share ?? 0, shares ?? 0);
    const missing = missingSwitches(readFileSync(binary), ids);
    if (missing.length > 0) {
      console.error(
        `${binary} has no switch for ${String(missing.length)} of the ${String(ids.length)} mutants, as ${missing[0] ?? ""}: Mull no longer switches them on by environment variable, so none would be tested.`,
      );
      process.exit(1);
    }
    if (!(await testsPass({ limitSeconds: limit, tests }))) {
      console.error(
        `The tests fail, or take more than ${String(limit)} s, with no mutant: every mutant would read as noticed.`,
      );
      process.exit(1);
    }
    writeFileSync(results, "");
    const done = await run({
      ids,
      results,
      limitSeconds: limit,
      deadlineSeconds: deadline,
      tests,
      workers,
    });
    const notRun = done.filter((result) => result.status === "NotRun").length;
    console.log(
      `${String(done.length - notRun)} mutants run, ${String(notRun)} not run.`,
    );
  } else if (command === "merge" && report !== undefined) {
    const results = args.slice(2).flatMap(readResults);
    console.log(JSON.stringify(merge(report, results)));
  } else {
    console.error(
      "Usage: node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl> --limit <s> --deadline <s> [--workers <n>] -- <binary> [args] [-- <binary> [args]]... | merge <dry-run.json> <results.jsonl>...",
    );
    process.exit(2);
  }
}
