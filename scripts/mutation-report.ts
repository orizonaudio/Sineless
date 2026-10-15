// Prints a mutation testing report (Stryker's or Mull's) as Markdown under a title, or with
// --annotate as GitHub Actions warnings on the lines of the mutants no test noticed, with paths
// relative to the working directory (scripts/mutation-summary.ts). With Mull's log and each
// mutant's time limit, the summary also says how long the tests take against that limit.
//   node scripts/mutation-report.ts <report.json> <title> [seconds the run took]
//       [--mull-log <mull.log> --limit <seconds>]
//   node scripts/mutation-report.ts <report.json> --annotate
import { readFileSync } from "node:fs";
import { parseArgs } from "node:util";

import {
  annotations,
  relativeTo,
  summarise,
  timeLimitNote,
  warmUpSeconds,
  type Report,
} from "./mutation-summary.ts";

const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: { "mull-log": { type: "string" }, limit: { type: "string" } },
});
const [path, option, seconds] = positionals;
if (path === undefined || option === undefined) {
  console.error(
    "Usage: node scripts/mutation-report.ts <report.json> <title> [seconds] | --annotate",
  );
  process.exit(2);
}
const report = relativeTo(
  JSON.parse(readFileSync(path, "utf8")) as Report,
  process.cwd(),
);
if (option === "--annotate") {
  console.log(annotations(report).join("\n"));
} else {
  let summary = summarise(
    report,
    option,
    seconds === undefined ? undefined : Number(seconds),
  );
  const log = values["mull-log"];
  const limit = values.limit;
  if (log !== undefined && limit !== undefined) {
    const warmUp = warmUpSeconds(readFileSync(log, "utf8"));
    if (warmUp !== undefined) {
      summary += `\n\n${timeLimitNote(warmUp, Number(limit))}`;
    }
  }
  console.log(summary);
}
