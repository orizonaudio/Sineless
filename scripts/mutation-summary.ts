// What a mutation testing report says (Stryker's for the TypeScript, Mull's for the C++, in the
// same format): the mutation score, each status's count and the mutants the tests did not notice,
// as Markdown and as GitHub Actions warnings.
import { isAbsolute, relative, sep } from "node:path";

import type { BaselineReport } from "./mutation-run.ts";

// The parts of the mutation testing report schema read here.
export interface Mutant {
  id?: string; // Mull's; Stryker's are numbers in its report, and unused here.
  mutatorName: string;
  replacement?: string;
  status: string;
  location: { start: { line: number; column: number } };
}

export interface Report {
  files: Record<string, { mutants: Mutant[] }>;
}

// Survived: run by a test that did not fail. NoCoverage: run by no test at all.
const unnoticed = new Set(["Survived", "NoCoverage"]);
// Noticed: a test failed, or the mutant hung a test (a timeout) or crashed it. The other statuses
// (a mutant that does not compile, or one left out) count neither way.
const noticed = new Set(["Killed", "Timeout", "RuntimeError"]);
// Pending: listed but not run, as the run reached its deadline or a share of it failed
// (scripts/mutation-run.ts).
const notRunStatus = "Pending";
// GitHub shows at most 10 warnings of a step on the pull request.
const maxAnnotations = 10;

// The mutants no test noticed, by file and line.
function missedMutants(report: Report): { file: string; mutant: Mutant }[] {
  return Object.entries(report.files)
    .flatMap(([file, { mutants }]) =>
      mutants
        .filter((mutant) => unnoticed.has(mutant.status))
        .map((mutant) => ({ file, mutant })),
    )
    .sort(
      (a, b) =>
        a.file.localeCompare(b.file) ||
        a.mutant.location.start.line - b.mutant.location.start.line,
    );
}

// How many mutants the report lists as not run: a partial report.
export function notRunCount(report: Report): number {
  return Object.values(report.files)
    .flatMap(({ mutants }) => mutants)
    .filter((mutant) => mutant.status === notRunStatus).length;
}

// The mutants no test noticed, one entry each, for the mutation triage (CI's mutation-triage
// workflow), which judges each by REVIEW.md's "Mutation testing".
export function survivors(report: Report): {
  file: string;
  line: number;
  column: number;
  mutator: string;
  replacement: string;
  status: string;
}[] {
  return missedMutants(report).map(({ file, mutant }) => ({
    file,
    line: mutant.location.start.line,
    column: mutant.location.start.column,
    mutator: mutant.mutatorName,
    replacement: mutant.replacement ?? "",
    status: mutant.status,
  }));
}

// One line of code, as Markdown table cells may not hold a line break or a bar.
function cell(text: string): string {
  return `\`${text.replaceAll(/\s+/gu, " ").replaceAll("|", "\\|")}\``;
}

// The report with its files' paths relative to `root`, with forward slashes as GitHub and the
// reports use: Mull writes them absolute.
export function relativeTo(report: Report, root: string): Report {
  return {
    files: Object.fromEntries(
      Object.entries(report.files).map(([file, mutants]) => [
        isAbsolute(file) ? relative(root, file).split(sep).join("/") : file,
        mutants,
      ]),
    ),
  };
}

// The report as Markdown under `title`; `seconds` is how long the run took, if known.
export function summarise(
  report: Report,
  title: string,
  seconds?: number,
): string {
  const counts = new Map<string, number>();
  for (const { mutants } of Object.values(report.files)) {
    for (const mutant of mutants) {
      counts.set(mutant.status, (counts.get(mutant.status) ?? 0) + 1);
    }
  }
  const count = (statuses: Set<string>) =>
    [...statuses].reduce(
      (total, status) => total + (counts.get(status) ?? 0),
      0,
    );
  const caught = count(noticed);
  const measured = caught + count(unnoticed);
  const notRun = counts.get(notRunStatus) ?? 0;
  const lines = [`## ${title}`, ""];
  if (measured === 0 && notRun === 0) {
    lines.push("No mutants to test.");
  } else {
    lines.push(
      measured === 0
        ? "No mutant was run."
        : `${String(caught)} of ${String(measured)} mutants noticed by the tests (${((100 * caught) / measured).toFixed(1)}%)` +
            (seconds === undefined ? "." : `, in ${String(seconds)} s.`),
      "",
      "| Status | Mutants |",
      "| --- | ---: |",
      ...[...counts].map(
        ([status, total]) => `| ${status} | ${String(total)} |`,
      ),
    );
  }
  if (notRun > 0) {
    lines.push(
      "",
      `**Partial:** ${String(notRun)} mutants were not run (the run reached its deadline, or a share of it failed); what they would show is unknown.`,
    );
  }
  const missed = missedMutants(report);
  const timedOut = Object.entries(report.files)
    .flatMap(([file, { mutants }]) =>
      mutants
        .filter((mutant) => mutant.status === "Timeout")
        .map((mutant) => ({ file, mutant })),
    )
    .sort(
      (a, b) =>
        a.file.localeCompare(b.file) ||
        a.mutant.location.start.line - b.mutant.location.start.line,
    );
  if (missed.length > 0) {
    lines.push(
      "",
      "Not noticed: each is a change to the code that no test fails on.",
      "",
      "| Where | Status | Change |",
      "| --- | --- | --- |",
      ...missed.map(
        ({ file, mutant }) =>
          `| ${file}:${String(mutant.location.start.line)} | ${mutant.status} | ${mutant.mutatorName}: ${cell(mutant.replacement ?? "")} |`,
      ),
    );
  }
  // A timeout counts as noticed; listed so that one a limit set too tight stopped, a survivor in
  // truth, shows.
  if (timedOut.length > 0) {
    lines.push(
      "",
      "Timed out: each hung its tests past its limit, which counts as noticed.",
      "",
      "| Where | Change |",
      "| --- | --- |",
      ...timedOut.map(
        ({ file, mutant }) =>
          `| ${file}:${String(mutant.location.start.line)} | ${mutant.mutatorName}: ${cell(mutant.replacement ?? "")} |`,
      ),
    );
  }
  return lines.join("\n");
}

// Text in a workflow command: GitHub decodes %XX, so `%` and line breaks are escaped, and in a
// property also the `:` and `,` that end it.
function commandData(text: string): string {
  return text
    .replaceAll("%", "%25")
    .replaceAll("\r", "%0D")
    .replaceAll("\n", "%0A");
}

function commandProperty(text: string): string {
  return commandData(text).replaceAll(":", "%3A").replaceAll(",", "%2C");
}

// GitHub Actions warnings for the first mutants no test noticed, shown on their lines.
export function annotations(report: Report): string[] {
  return missedMutants(report)
    .slice(0, maxAnnotations)
    .map(({ file, mutant }) => {
      const { line, column } = mutant.location.start;
      const change = (mutant.replacement ?? "").replaceAll(/\s+/gu, " ");
      return (
        `::warning file=${commandProperty(file)},line=${String(line)},col=${String(column)},` +
        `title=${commandProperty(`Mutant ${mutant.status.toLowerCase()}`)}::` +
        commandData(
          `No test fails when this is changed (${mutant.mutatorName}): ${change}`,
        )
      );
    });
}

// What the runs with no mutant measured, per test binary (scripts/mutation-run.ts, --baseline-out):
// its tests' time and each mutant's limit there, the largest any share measured (each runs all the
// binary's tests on its own runner), with the rule the limits follow. A binary in `expected`
// (one with mutants) that no share timed shows "—": its run failed, or no share held its mutants.
export function baselineNote(
  reports: BaselineReport[],
  expected: string[],
): string {
  const largest = new Map<string, { seconds: number; limitSeconds: number }>();
  for (const { binaries } of reports) {
    for (const { name, seconds, limitSeconds } of binaries) {
      const seen = largest.get(name);
      largest.set(name, {
        seconds: Math.max(seconds, seen?.seconds ?? 0),
        limitSeconds: Math.max(limitSeconds, seen?.limitSeconds ?? 0),
      });
    }
  }
  const names = [...new Set([...expected, ...largest.keys()])];
  if (names.length === 0) {
    return "";
  }
  const [first] = reports;
  const lines =
    first === undefined
      ? []
      : [
          `Each mutant is stopped at ${String(first.factor)} × its tests' time with no mutant + ${String(first.constantSeconds)} s, the time measured with every worker busy.`,
          "",
        ];
  lines.push(
    "| Tests | With no mutant | Each mutant stopped at |",
    "| --- | ---: | ---: |",
    ...names.map((name) => {
      const times = largest.get(name);
      return times === undefined
        ? `| ${name} | — | — |`
        : `| ${name} | ${times.seconds.toFixed(0)} s | ${times.limitSeconds.toFixed(0)} s |`;
    }),
  );
  return lines.join("\n");
}

// Whether Stryker found no unit test reaching the lines it mutated: with allowEmpty
// (tools/mutation/stryker.config.json) it then ends without a report, its dry run having said "No
// tests were found". A run that left no report for any other reason did not complete.
// Its log is coloured even when written to a file: the colour codes are dropped first.
export function noTestReached(strykerLog: string, hasReport: boolean): boolean {
  // eslint-disable-next-line no-control-regex -- colour codes begin with the escape character.
  const plain = strykerLog.replaceAll(/\u001b\[[0-9;]*m/gu, "");
  return !hasReport && /DryRunExecutor No tests were found$/mu.test(plain);
}

// The summary of such a run: nothing to judge, as the mutation triage reads it
// (scripts/mutation-triage.ts). The lines are left to the tests that do reach them, such as the
// app tests, which drive real browsers and so do not run once per mutant.
export function noTestReachedSummary(title: string, base: string): string {
  return `## ${title}\n\nNo unit test reaches the lines changed since ${base}: they are left to the app tests.`;
}

// Whether Mull's dry run found no mutant on the lines it was given, as when only comments of the
// engine changed: it then writes no list (scripts/mutation-cpp.sh). Its log says so; a dry run
// that wrote no list for any other reason did not complete.
export function noMutantFound(mullLog: string): boolean {
  return /^\[info\] No mutants found\./mu.test(mullLog);
}

// The summary of such a run: nothing to judge, as the mutation triage reads it
// (scripts/mutation-triage.ts).
export function noMutantSummary(title: string, base: string): string {
  return `## ${title}\n\nNo mutant on the lines changed since ${base}.`;
}
