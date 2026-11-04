// Prints a mutation testing report (Stryker's or Mull's) as Markdown under a title, or with
// --annotate as GitHub Actions warnings on the lines of the mutants no test noticed, with paths
// relative to the working directory (scripts/mutation-summary.ts). With the C++ runs' times with
// no mutant (scripts/mutation-run.ts, --baseline-out), the summary also says how long each test
// binary's tests take and where each mutant is stopped.
//   node scripts/mutation-report.ts <report.json> <title> [seconds the run took]
//       [--baseline <baseline.json>]... [--tests <test binary>]...
// (--tests: each binary with mutants, shown as "—" if no --baseline file times it)
//   node scripts/mutation-report.ts <report.json> --annotate
//   node scripts/mutation-report.ts <report.json> --survivors   (the mutants no test noticed, JSON)
import { existsSync, readFileSync } from "node:fs";
import { parseArgs } from "node:util";

import type { BaselineReport } from "./mutation-run.ts";
import {
  annotations,
  noMutantFound,
  noMutantSummary,
  noTestReached,
  noTestReachedSummary,
  relativeTo,
  summarise,
  survivors,
  baselineNote,
  type Report,
} from "./mutation-summary.ts";

// Every option the callers pass (scripts/mutation.sh, scripts/mutation-cpp.sh, CI's Report steps)
// is declared: parseArgs refuses any other.
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: {
    annotate: { type: "boolean" },
    survivors: { type: "boolean" },
    baseline: { type: "string", multiple: true },
    tests: { type: "string", multiple: true },
    "stryker-log": { type: "string" },
    "mull-dry-log": { type: "string" },
    base: { type: "string" },
  },
});
const [path, title, seconds] = positionals;
const option =
  values.annotate === true
    ? "--annotate"
    : values.survivors === true
      ? "--survivors"
      : title;
if (path === undefined || option === undefined) {
  console.error(
    "Usage: node scripts/mutation-report.ts <report.json> <title> [seconds] | --annotate | --survivors",
  );
  process.exit(2);
}
// With Stryker's log: a run that wrote no report because no unit test reaches the lines it mutated
// is summarised as such; one that wrote none for any other reason fails here, as before.
const strykerLog = values["stryker-log"];
if (strykerLog !== undefined && option === title && !existsSync(path)) {
  if (!noTestReached(readFileSync(strykerLog, "utf8"), false)) {
    console.error(
      `Stryker wrote no report (${path}); see its log, ${strykerLog}.`,
    );
    process.exit(1);
  }
  console.log(noTestReachedSummary(title, values.base ?? "the base"));
  process.exit(0);
}
// With Mull's dry-run log: a dry run that wrote no list because no mutant is on the lines changed is
// summarised as such; one that wrote none for any other reason fails here.
const mullDryLog = values["mull-dry-log"];
if (mullDryLog !== undefined && option === title && !existsSync(path)) {
  if (!noMutantFound(readFileSync(mullDryLog, "utf8"))) {
    console.error(`Mull wrote no list (${path}); see its log, ${mullDryLog}.`);
    process.exit(1);
  }
  console.log(noMutantSummary(title, values.base ?? "the base"));
  process.exit(0);
}
const report = relativeTo(
  JSON.parse(readFileSync(path, "utf8")) as Report,
  process.cwd(),
);
if (values.survivors === true) {
  console.log(JSON.stringify(survivors(report)));
} else if (option === "--annotate") {
  console.log(annotations(report).join("\n"));
} else {
  let summary = summarise(
    report,
    option,
    seconds === undefined ? undefined : Number(seconds),
  );
  const baselines = (values.baseline ?? []).map(
    (file) => JSON.parse(readFileSync(file, "utf8")) as BaselineReport,
  );
  const note = baselineNote(baselines, values.tests ?? []);
  if (note !== "") {
    summary += `\n\n${note}`;
  }
  console.log(summary);
}
