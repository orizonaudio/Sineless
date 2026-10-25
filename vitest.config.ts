import { fileURLToPath } from "node:url";

import { playwright } from "@vitest/browser-playwright";
import { defineConfig } from "vitest/config";

import { unitTests } from "./vitest.unit.ts";

// The app tests run the real page in WebKit (as WKWebView does on macOS) and Chromium (as
// WebView2 does on Windows). CHROMIUM_EXECUTABLE runs them in Chromium alone, with an installed
// browser, where Playwright cannot download its own; APP_BROWSERS=chromium does too, as coverage
// needs (V8 coverage is measured in Chromium only).
const chromium = process.env["CHROMIUM_EXECUTABLE"];
const chromiumOnly =
  chromium !== undefined || process.env["APP_BROWSERS"] === "chromium";

export default defineConfig({
  test: {
    // The product's sources, for `vitest run --coverage` (see scripts/coverage.sh).
    coverage: {
      provider: "v8",
      include: ["protocol/src/**/*.ts", "ui/src/**/*.ts"],
    },
    projects: [
      { test: unitTests },
      {
        // The page's brand, as an official build has it (ui/vite.config.ts).
        resolve: {
          alias: {
            "@brand": fileURLToPath(new URL("./ui/src/brand", import.meta.url)),
          },
        },
        test: {
          name: "app",
          include: ["ui/tests/app/**/*.test.ts"],
          // One page at a time: a page out of view gets no animation frames, which draw it.
          fileParallelism: false,
          // The page draws on animation frames; a busy machine draws them late.
          expect: { poll: { timeout: 5000 } },
          testTimeout: 30000,
          browser: {
            enabled: true,
            headless: true,
            // Clicks wait up to 10 s for the page, well within a test's 30 s: under coverage,
            // or on a busy machine, the page draws late.
            provider: playwright({
              actionTimeout: 10000,
              ...(chromium === undefined
                ? {}
                : { launchOptions: { executablePath: chromium } }),
            }),
            instances: chromiumOnly
              ? [{ browser: "chromium" }]
              : [{ browser: "webkit" }, { browser: "chromium" }],
            // Wider than the editor's minimum width, as a host opens it.
            viewport: { width: 1200, height: 720 },
          },
        },
      },
    ],
  },
});
