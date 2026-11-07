// The linux check (.github/workflows/ci.yml), which main's ruleset requires, over the jobs that
// share the AddressSanitizer tests: it passes when they passed, or were skipped as a whole (a
// change to documentation only), and fails otherwise, a cancelled shard included.
//   RESULT=<needs.linux-asan.result> node scripts/ci-gate.ts
const result = process.env["RESULT"] ?? "";
if (result === "success" || result === "skipped") {
  console.log(`The AddressSanitizer shards' result: ${result}.`);
} else {
  console.error(
    `The AddressSanitizer shards' result is '${result}'; see their jobs.`,
  );
  process.exit(1);
}
