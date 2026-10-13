// Prints a mutation testing report (Stryker's or Mull's) as Markdown under a title, or with
// --annotate as GitHub Actions warnings on the lines of the mutants no test noticed, with paths
// relative to the working directory (scripts/mutation-summary.ts).
//   node scripts/mutation-report.ts <report.json> <title> [seconds the run took]
//   node scripts/mutation-report.ts <report.json> --annotate
import { readFileSync } from "node:fs";

import {
  annotations,
  relativeTo,
  summarise,
  type Report,
} from "./mutation-summary.ts";

const [path, option, seconds] = process.argv.slice(2);
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
  console.log(
    summarise(
      report,
      option,
      seconds === undefined ? undefined : Number(seconds),
    ),
  );
}
