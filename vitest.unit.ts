// The unit tests, in Node: run by vitest.config.ts, and by mutation testing
// (vitest.mutation.config.ts), which runs them with a Vitest of its own, so nothing here may import
// from Vitest.
export const unitTests = {
  name: "unit",
  include: [
    "protocol/tests/**/*.test.ts",
    "ui/tests/**/*.test.ts",
    "scripts/tests/**/*.test.ts",
  ],
  exclude: ["ui/tests/app/**"],
};
