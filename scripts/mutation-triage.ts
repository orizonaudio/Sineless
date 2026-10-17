// The mutation triage's rules (CI's mutation-triage workflow): what each mutant's verdict is once
// the triage and the verification have answered, whether the check passes, and the comment that
// says so. Kept here rather than in the workflow, so that the rules that decide the check have
// tests.
//   node scripts/mutation-triage.ts number <mutants.json>   (numbered, with repository paths)
//   node scripts/mutation-triage.ts outcome <job conclusion> <report.json> [summary.md]
//   node scripts/mutation-triage.ts not-run <report.json>   (how many mutants it did not run)
//   node scripts/mutation-triage.ts judge <mutants.json> <triage.json>   (writes gaps to verify)
//   node scripts/mutation-triage.ts report <judged.json> <verify.json> [mutants not run]
//       (prints the check's state and the comment)
import { existsSync, readFileSync } from "node:fs";

import { notRunCount, type Report } from "./mutation-summary.ts";

export interface Mutant {
  id: number;
  file: string;
  line: number;
  mutator: string;
  replacement: string;
}

// The triage's verdicts (.claude/agents/mutation-triager.md); the workflow's output schema allows
// these, and a test checks it allows no other. Only a gap goes on to a verifier.
export const verdicts = [
  "gap",
  "equivalent",
  "speed",
  "tuned",
  "dead",
  "message",
  "tested",
] as const;
export type Verdict = (typeof verdicts)[number];

export interface Judged extends Mutant {
  verdict: Verdict | "not a gap";
  reason: string;
  proof?: string;
}

interface TriageVerdict {
  id: number;
  verdict: Verdict;
  reason: string;
}

interface Verification {
  id: number;
  verified: boolean;
  severity: "important" | "nit";
  proof: string;
}

// What a mutation job of the CI run left to triage: nothing (it did not run, or no source line it
// mutates changed), its report, a report with some mutants not run (the run reached its deadline
// first, scripts/mutation-run.ts), or nothing although it ran (it failed, timed out or was
// cancelled). An incomplete or partial run must never read as one with no mutant left unnoticed.
export function runOutcome(
  conclusion: string,
  hasReport: boolean,
  summary: string | undefined,
  notRun = 0,
): "none" | "report" | "partial" | "incomplete" {
  if (conclusion === "" || conclusion === "skipped") {
    return "none";
  }
  if (hasReport) {
    return notRun > 0 ? "partial" : "report";
  }
  if (
    summary !== undefined &&
    /^No (TypeScript|engine) source lines changed since |^No unit test reaches the lines changed since |^No mutant on the lines changed since /mu.test(
      summary,
    )
  ) {
    return "none";
  }
  return "incomplete";
}

// A report's file as a path in the repository: the reports name files from the workspace CI ran
// in, and one made elsewhere (in Docker, or another checkout) is cut back to the first of the
// repository's source folders.
export function repositoryPath(file: string): string {
  if (!file.startsWith("/") && !file.startsWith("../")) {
    return file;
  }
  const match =
    /(?:^|\/)((?:engine|plugin|editor|protocol|ui|scripts)\/.*)$/u.exec(file);
  return match?.[1] ?? file;
}

// The mutants of every report, numbered, with repository paths.
export function numbered(mutants: Omit<Mutant, "id">[]): Mutant[] {
  return mutants.map((mutant, id) => ({
    ...mutant,
    id,
    file: repositoryPath(mutant.file),
  }));
}

// Each mutant with the triage's verdict. A mutant left without one counts as a gap: unjudged, it
// must not pass.
export function judge(mutants: Mutant[], verdicts: TriageVerdict[]): Judged[] {
  return mutants.map((mutant) => {
    const found = verdicts.find((verdict) => verdict.id === mutant.id);
    return {
      ...mutant,
      verdict: found?.verdict ?? "gap",
      reason: found?.reason ?? "No verdict was returned.",
    };
  });
}

// The gaps, as findings for a review-verifier.
export function gapsToVerify(judged: Judged[]) {
  return judged
    .filter((mutant) => mutant.verdict === "gap")
    .map((mutant) => ({
      id: mutant.id,
      file: mutant.file,
      line: mutant.line,
      title: `No test notices ${change(mutant, "the call removed")} (${mutant.mutator}) at ${mutant.file}:${String(mutant.line)}`,
      scenario: mutant.reason,
    }));
}

// The verdicts once verified: a gap stays one only if its verifier confirms it at Important, or
// returned nothing; one rejected or rated a nit is not a gap.
export function verify(
  judged: Judged[],
  verifications: Verification[],
): Judged[] {
  return judged.map((mutant) => {
    if (mutant.verdict !== "gap") {
      return mutant;
    }
    const found = verifications.find((v) => v.id === mutant.id);
    if (found === undefined) {
      return {
        ...mutant,
        proof: "No verification was returned; kept as a gap.",
      };
    }
    if (!found.verified || found.severity !== "important") {
      return { ...mutant, verdict: "not a gap", proof: found.proof };
    }
    return { ...mutant, proof: found.proof };
  });
}

// The check fails while a gap is left.
export function gapsLeft(result: Judged[]): number {
  return result.filter((mutant) => mutant.verdict === "gap").length;
}

// The check's state: a failure while a gap is left; otherwise an error while some mutants were
// not run, as what they would show is unknown; a success only when every mutant ran and no gap is
// left.
export function checkState(
  gaps: number,
  total: number,
  notRun: number,
): { state: "success" | "failure" | "error"; description: string } {
  const unrun = notRun > 0 ? `; ${String(notRun)} not run` : "";
  if (gaps > 0) {
    return {
      state: "failure",
      description: `${String(gaps)} untested gap(s) among ${String(total)} mutant(s)${unrun}`,
    };
  }
  if (notRun > 0) {
    return {
      state: "error",
      description: `No untested gap among ${String(total)} mutant(s)${unrun}`,
    };
  }
  return {
    state: "success",
    description:
      total === 0
        ? "No mutant left unnoticed"
        : `No untested gap among ${String(total)} mutant(s)`,
  };
}

// A mutant's change as Markdown: a removed call has no replacement.
function change(mutant: Mutant, removed = "removed"): string {
  return mutant.replacement === ""
    ? removed
    : `\`${mutant.replacement.replaceAll(/\s+/gu, " ").replaceAll("|", "\\|")}\``;
}

function oneLine(text: string): string {
  return text.replaceAll(/\s+/gu, " ").replaceAll("|", "\\|");
}

// The comment on the pull request.
export function comment(result: Judged[], notRun = 0): string {
  const gaps = result.filter((mutant) => mutant.verdict === "gap");
  const partial =
    notRun > 0
      ? [
          "",
          `**Partial:** ${String(notRun)} mutant(s) were not run (the mutation run reached its deadline, or a share of it failed), so the check cannot pass. Rerun the mutation jobs, or waive the check.`,
        ]
      : [];
  const lines =
    gaps.length > 0
      ? [
          "<!-- mutation-triage: failed -->",
          `### Mutation triage: ${String(gaps.length)} untested gap(s)`,
          "",
          "Of the mutants no test noticed on this pull request's changed lines, these would let a user see something go wrong. Add a test that fails with each, or answer it here.",
          "",
          ...gaps.map(
            (gap) =>
              `- **Gap** \`${gap.file}:${String(gap.line)}\` (${gap.mutator}: ${change(gap)}): ${oneLine(gap.reason)}\n  Verification: ${oneLine(gap.proof ?? "")}`,
          ),
          ...partial,
        ]
      : notRun > 0
        ? [
            "<!-- mutation-triage: partial -->",
            "### Mutation triage: no untested gap among the mutants run",
            ...partial,
          ]
        : [
            "<!-- mutation-triage: passed -->",
            "### Mutation triage: no untested gap",
          ];
  lines.push(
    "",
    `<details><summary>Every mutant no test noticed (${String(result.length)})</summary>`,
    "",
    "| Where | Change | Verdict | Why |",
    "| --- | --- | --- | --- |",
    ...result.map(
      (mutant) =>
        `| \`${mutant.file}:${String(mutant.line)}\` | ${mutant.mutator}: ${change(mutant)} | ${mutant.verdict} | ${oneLine(mutant.proof ?? mutant.reason)} |`,
    ),
    "",
    "</details>",
    "",
    "A maintainer can comment `/mutation-triage waive <reason>` to pass the check.",
  );
  return lines.join("\n");
}

const read = (path: string): unknown => JSON.parse(readFileSync(path, "utf8"));

if (process.argv[1]?.endsWith("mutation-triage.ts") === true) {
  const [command, first, second, third] = process.argv.slice(2);
  if (command === "number" && first !== undefined) {
    // number <mutants.json>
    console.log(JSON.stringify(numbered(read(first) as Omit<Mutant, "id">[])));
  } else if (
    command === "outcome" &&
    first !== undefined &&
    second !== undefined
  ) {
    // outcome <job conclusion> <report.json> [summary.md]
    const summaryPath = third;
    const hasReport = existsSync(second);
    console.log(
      runOutcome(
        first,
        hasReport,
        summaryPath !== undefined && existsSync(summaryPath)
          ? readFileSync(summaryPath, "utf8")
          : undefined,
        hasReport ? notRunCount(read(second) as Report) : 0,
      ),
    );
  } else if (command === "not-run" && first !== undefined) {
    console.log(notRunCount(read(first) as Report));
  } else if (
    command === "judge" &&
    first !== undefined &&
    second !== undefined
  ) {
    const judged = judge(
      read(first) as Mutant[],
      (read(second) as { verdicts: TriageVerdict[] }).verdicts,
    );
    console.log(JSON.stringify({ judged, gaps: gapsToVerify(judged) }));
  } else if (
    command === "report" &&
    first !== undefined &&
    second !== undefined
  ) {
    const result = verify(
      read(first) as Judged[],
      (read(second) as { verdicts: Verification[] }).verdicts,
    );
    const notRun = Number(third ?? 0);
    console.log(
      JSON.stringify({
        gaps: gapsLeft(result),
        total: result.length,
        ...checkState(gapsLeft(result), result.length, notRun),
        comment: comment(result, notRun),
      }),
    );
  } else {
    console.error(
      "Usage: node scripts/mutation-triage.ts number <mutants.json> | outcome <conclusion> <report.json> [summary.md] | not-run <report.json> | judge <mutants.json> <triage.json> | report <judged.json> <verify.json> [not run]",
    );
    process.exit(2);
  }
}
