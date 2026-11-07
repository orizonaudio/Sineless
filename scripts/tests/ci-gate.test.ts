// The linux gate (scripts/ci-gate.ts): the check main's ruleset requires, over the four shards of
// the AddressSanitizer tests, passes only when they passed or did not need to run.
import { spawnSync } from "node:child_process";
import { resolve } from "node:path";
import { describe, expect, test } from "vitest";

import { variables } from "./check-harness.ts";

const gate = (result: string) => {
  const run = spawnSync(
    process.execPath,
    [resolve(import.meta.dirname, "../ci-gate.ts")],
    {
      encoding: "utf8",
      env: { ...process.env, ...variables(["RESULT", result]) },
    },
  );
  return { status: run.status, output: run.stdout + run.stderr };
};

describe("the linux gate", () => {
  test.each(["success", "skipped"])(
    "passes when the shards' result is %s",
    (result) => {
      expect(gate(result).status).toBe(0);
    },
  );

  test.each(["failure", "cancelled", ""])(
    "fails, naming it, when the result is '%s'",
    (result) => {
      const { status, output } = gate(result);
      expect(status).toBe(1);
      expect(output).toContain(`'${result}'`);
    },
  );
});
