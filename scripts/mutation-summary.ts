// What a mutation testing report says (Stryker's for the TypeScript, Mull's for the C++, in the
// same format): the mutation score, each status's count and the mutants the tests did not notice,
// as Markdown and as GitHub Actions warnings.
import { isAbsolute, relative, sep } from "node:path";

// The parts of the mutation testing report schema read here.
export interface Mutant {
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
  const lines = [`## ${title}`, ""];
  if (measured === 0) {
    lines.push("No mutants to test.");
  } else {
    lines.push(
      `${String(caught)} of ${String(measured)} mutants noticed by the tests (${((100 * caught) / measured).toFixed(1)}%)` +
        (seconds === undefined ? "." : `, in ${String(seconds)} s.`),
      "",
      "| Status | Mutants |",
      "| --- | ---: |",
      ...[...counts].map(
        ([status, total]) => `| ${status} | ${String(total)} |`,
      ),
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
