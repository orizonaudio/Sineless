// The mutation triage's rules (CI's mutation-triage workflow): what each mutant's verdict is once
// the triage and the verification have answered, whether the check passes, and the comment that
// says so. Kept here rather than in the workflow, so that the rules that decide the check have
// tests.
//   node scripts/mutation-triage.ts outcome <job conclusion> <report.json> [summary.md]
//   node scripts/mutation-triage.ts judge <mutants.json> <triage.json>   (writes gaps to verify)
//   node scripts/mutation-triage.ts report <judged.json> <verify.json>   (prints the comment)
import { existsSync, readFileSync } from "node:fs";

export interface Mutant {
  id: number;
  file: string;
  line: number;
  mutator: string;
  replacement: string;
}

export type Verdict =
  "gap" | "equivalent" | "speed" | "tuned" | "dead" | "message";

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
// mutates changed), its report, or nothing although it ran (it failed, timed out, was cancelled,
// or stopped before the end, as scripts/mutation-cpp.sh does after 75 minutes). An incomplete run
// must never read as one with no mutant left unnoticed.
export function runOutcome(
  conclusion: string,
  hasReport: boolean,
  summary: string | undefined,
): "none" | "report" | "incomplete" {
  if (conclusion === "" || conclusion === "skipped") {
    return "none";
  }
  if (hasReport) {
    return "report";
  }
  if (
    summary !== undefined &&
    /^No (TypeScript|engine) source lines changed since /mu.test(summary)
  ) {
    return "none";
  }
  return "incomplete";
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
export function comment(result: Judged[]): string {
  const gaps = result.filter((mutant) => mutant.verdict === "gap");
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
  if (command === "outcome" && first !== undefined && second !== undefined) {
    // outcome <job conclusion> <report.json> [summary.md]
    const summaryPath = third;
    console.log(
      runOutcome(
        first,
        existsSync(second),
        summaryPath !== undefined && existsSync(summaryPath)
          ? readFileSync(summaryPath, "utf8")
          : undefined,
      ),
    );
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
    console.log(
      JSON.stringify({
        gaps: gapsLeft(result),
        total: result.length,
        comment: comment(result),
      }),
    );
  } else {
    console.error(
      "Usage: node scripts/mutation-triage.ts judge <mutants.json> <triage.json> | report <judged.json> <verify.json>",
    );
    process.exit(2);
  }
}
