// What a mutation testing report says (Stryker's for the TypeScript, Mull's for the C++, in the
// same format): the mutation score, each status's count and the mutants the tests did not notice,
// as Markdown and as GitHub Actions warnings.
import { isAbsolute, relative, sep } from "node:path";

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

// A duration as Mull's progress lines print it ("216ms", "58.2s", "4m40.4s", "1h2m3s"), in
// seconds; undefined if it is not one.
export function mullSeconds(text: string): number | undefined {
  const units: Record<string, number> = { h: 3600, m: 60, s: 1, ms: 0.001 };
  const parts = [...text.matchAll(/(\d+(?:\.\d+)?)(ms|h|m|s)/gu)];
  if (parts.length === 0 || parts.map((part) => part[0]).join("") !== text) {
    return undefined;
  }
  return parts.reduce(
    (total, [, amount, unit]) =>
      total + Number(amount) * (units[unit ?? ""] ?? 0),
    0,
  );
}

// How long Mull's warm-up run, the tests with no mutant, took: the duration on the progress line
// after "Warm up run" in its log, blank lines between. Undefined when the log has none.
export function warmUpSeconds(log: string): number | undefined {
  const match = /Warm up run.*\n(?:[ \t]*\n)*.*Finished in (\S+)/u.exec(log);
  return match?.[1] === undefined ? undefined : mullSeconds(match[1]);
}

// How long the tests take against each mutant's time limit. A mutant no test notices runs every
// test: past half the limit, the margin is too small, as one would be stopped and reported as
// timed out, which counts as noticed and hides a line no test checks.
export function timeLimitNote(warmUp: number, limit: number): string {
  const lines = [
    `The tests take ${warmUp.toFixed(0)} s with no mutant; each mutant is stopped at ${limit.toFixed(0)} s.`,
  ];
  if (warmUp * 2 > limit) {
    lines.push(
      "",
      "> [!WARNING]",
      "> The tests take more than half the time limit: raise MUTANT_TIMEOUT, or mutants no test notices may be reported as timed out.",
    );
  }
  return lines.join("\n");
}
