// Runs the C++ mutants Mull built into the test binaries, a share at a time, in place of
// mull-runner: mull-runner has no way to split its mutants between jobs, runs one test binary,
// writes nothing until every mutant has run, and limits each mutant only to ten times the tests'
// own time. Here each mutant runs as Mull runs it (its module's test binary with an environment
// variable named after the mutant set to 1), its result is written as soon as it is known, a
// mutant that takes longer than its module's limit is stopped and counted as timed out, and the
// mutants not reached by the deadline are reported as not run.
//   node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl>
//       --deadline <seconds in all> --tests <module> <seconds a mutant> <test binary> [--tests ...]
//       [-- <arguments for every test binary>]
//   node scripts/mutation-run.ts combine <dry-run.json>...   (prints them as one list)
//   node scripts/mutation-run.ts merge <dry-run.json> <results.jsonl>...   (prints the report)
// A dry run is Mull's report of every mutant with no test run (mull-runner --dry-run); there is
// one for each module's test binary, with the mutants of that module's sources.
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

// Each mutant's file, by id.
export function mutantFiles(report: Report): Map<string, string> {
  return new Map(
    Object.entries(report.files).flatMap(([file, { mutants }]) =>
      (mutants as ReportMutant[]).map((mutant) => [mutant.id, file] as const),
    ),
  );
}

// The dry runs of several modules' test binaries as one list. Each lists the mutants in its own
// module's files, so no file is in two.
export function combine(reports: Report[]): Report {
  return {
    ...reports[0],
    files: Object.fromEntries(
      reports.flatMap((report) => Object.entries(report.files)),
    ),
  };
}

// A test binary, with its arguments, and how long it may run with one mutant.
export interface Tests {
  command: string;
  args: string[];
  limitSeconds: number;
}

// The tests a mutant runs against: its module's, the module whose sources (`<module>/src/` or
// `<module>/include/`) hold its file. Only those: run against the other test binaries that hold
// them, the mutants their own module's tests miss go unnoticed all the same (measured on the
// plugin's, the editor's and the engine's).
export function testsFor(
  file: string,
  tests: Map<string, Tests>,
): Tests | undefined {
  for (const [module, moduleTests] of tests) {
    const folders = [`${module}/src/`, `${module}/include/`];
    if (
      folders.some(
        (folder) => file.startsWith(folder) || file.includes(`/${folder}`),
      )
    ) {
      return moduleTests;
    }
  }
  return undefined;
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

interface RunOptions {
  mutants: { id: string; tests: Tests }[];
  results: string;
  deadlineSeconds: number;
  workers?: number;
}

// The tests run with the mutant `id` switched on, or with none.
function runTests(tests: Tests, id?: string): Promise<Result["status"]> {
  return new Promise((resolve) => {
    const child = spawn(tests.command, tests.args, {
      env: id === undefined ? process.env : { ...process.env, [id]: "1" },
      stdio: "ignore",
    });
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, tests.limitSeconds * 1000);
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
export async function testsPass(tests: Tests): Promise<boolean> {
  return (await runTests(tests)) === "Survived";
}

// Runs the mutants on every core, appending each result as it is known. Once the deadline has
// passed no new mutant starts, and those left are written as not run.
export async function run(options: RunOptions): Promise<Result[]> {
  const deadline = Date.now() + options.deadlineSeconds * 1000;
  const queue = [...options.mutants];
  const done: Result[] = [];
  const record = (result: Result) => {
    done.push(result);
    appendFileSync(options.results, `${JSON.stringify(result)}\n`);
  };
  const worker = async () => {
    for (
      let mutant = queue.shift();
      mutant !== undefined;
      mutant = queue.shift()
    ) {
      const { id, tests } = mutant;
      record({
        id,
        status: Date.now() < deadline ? await runTests(tests, id) : "NotRun",
      });
    }
  };
  await Promise.all(
    Array.from({ length: options.workers ?? availableParallelism() }, () =>
      worker(),
    ),
  );
  return done;
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
  const option = (name: string) => {
    const at = args.indexOf(name);
    return at === -1 ? undefined : args[at + 1];
  };
  const [command, dryRun] = args;
  const report =
    dryRun === undefined
      ? undefined
      : (JSON.parse(readFileSync(dryRun, "utf8")) as Report);
  if (command === "run" && report !== undefined) {
    const [share, shares] = (args[2] ?? "").split("/").map(Number);
    const results = args[3];
    const deadline = Number(option("--deadline"));
    const separator = args.indexOf("--");
    const common = separator === -1 ? [] : args.slice(separator + 1);
    const options = separator === -1 ? args : args.slice(0, separator);
    // Each --tests names a module, its limit in seconds and its test binary.
    const tests = new Map<string, Tests>();
    let malformed = false;
    for (const [at, arg] of options.entries()) {
      if (arg === "--tests") {
        const [module, limit, binary] = options.slice(at + 1, at + 4);
        if (module !== undefined && binary !== undefined && Number(limit) > 0) {
          tests.set(module, {
            command: binary,
            args: common,
            limitSeconds: Number(limit),
          });
        } else {
          malformed = true;
        }
      }
    }
    if (
      results === undefined ||
      tests.size === 0 ||
      malformed ||
      !(deadline > 0)
    ) {
      console.error(
        "run needs <share>/<shares> <results.jsonl> --deadline <s> --tests <module> <s> <binary>",
      );
      process.exit(2);
    }
    const files = mutantFiles(report);
    const mutants = shareOf(mutantIds(report), share ?? 0, shares ?? 0).map(
      (id) => ({
        id,
        file: files.get(id) ?? "",
        tests: testsFor(files.get(id) ?? "", tests),
      }),
    );
    const untested = mutants.filter((mutant) => mutant.tests === undefined);
    if (untested[0] !== undefined) {
      console.error(
        `No tests for ${String(untested.length)} mutant(s), as ${untested[0].id} in ${untested[0].file}: give their module's with --tests.`,
      );
      process.exit(1);
    }
    // Each test binary this share runs: it must switch each of its mutants on, and pass with none.
    for (const moduleTests of new Set(mutants.map((mutant) => mutant.tests))) {
      if (moduleTests === undefined) {
        continue;
      }
      const binary = moduleTests.command;
      const ids = mutants
        .filter((mutant) => mutant.tests === moduleTests)
        .map((mutant) => mutant.id);
      const missing = missingSwitches(readFileSync(binary), ids);
      if (missing.length > 0) {
        console.error(
          `${binary} has no switch for ${String(missing.length)} of the ${String(ids.length)} mutants, as ${missing[0] ?? ""}: Mull no longer switches them on by environment variable, so none would be tested.`,
        );
        process.exit(1);
      }
      if (!(await testsPass(moduleTests))) {
        console.error(
          `${binary} fails, or takes more than ${String(moduleTests.limitSeconds)} s, with no mutant: every mutant would read as noticed.`,
        );
        process.exit(1);
      }
    }
    writeFileSync(results, "");
    const done = await run({
      mutants: mutants.flatMap(({ id, tests: moduleTests }) =>
        moduleTests === undefined ? [] : [{ id, tests: moduleTests }],
      ),
      results,
      deadlineSeconds: deadline,
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
      "Usage: node scripts/mutation-run.ts run <dry-run.json> <share>/<shares> <results.jsonl> --deadline <s> --tests <module> <s> <binary>... [-- args] | combine <dry-run.json>... | merge <dry-run.json> <results.jsonl>...",
    );
    process.exit(2);
  }
}
