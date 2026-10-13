// Prints a Stryker report (build/mutation/mutation.json) as Markdown, or with --annotate as GitHub
// Actions warnings on the lines of the mutants no test noticed (scripts/mutation-summary.ts).
//   node scripts/mutation-report.ts <mutation.json> [seconds Stryker took | --annotate]
import { readFileSync } from "node:fs";

import { annotations, summarise, type Report } from "./mutation-summary.ts";

const [path, option] = process.argv.slice(2);
if (path === undefined) {
  console.error(
    "Usage: node scripts/mutation-report.ts <mutation.json> [seconds | --annotate]",
  );
  process.exit(2);
}
const report = JSON.parse(readFileSync(path, "utf8")) as Report;
if (option === "--annotate") {
  console.log(annotations(report).join("\n"));
} else {
  console.log(
    summarise(report, option === undefined ? undefined : Number(option)),
  );
}
