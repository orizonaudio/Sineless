import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["protocol/tests/**/*.test.ts", "ui/tests/**/*.test.ts"],
  },
});
