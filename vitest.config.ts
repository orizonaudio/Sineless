import { playwright } from "@vitest/browser-playwright";
import { defineConfig } from "vitest/config";

// The app tests run the real page in WebKit (as WKWebView does on macOS) and Chromium (as
// WebView2 does on Windows). CHROMIUM_EXECUTABLE runs them in Chromium alone, with an installed
// browser, where Playwright cannot download its own.
const chromium = process.env["CHROMIUM_EXECUTABLE"];

export default defineConfig({
  test: {
    projects: [
      {
        test: {
          name: "unit",
          include: ["protocol/tests/**/*.test.ts", "ui/tests/**/*.test.ts"],
          exclude: ["ui/tests/app/**"],
        },
      },
      {
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
            provider: playwright(
              chromium === undefined
                ? {}
                : { launchOptions: { executablePath: chromium } },
            ),
            instances:
              chromium === undefined
                ? [{ browser: "webkit" }, { browser: "chromium" }]
                : [{ browser: "chromium" }],
            // Wider than the editor's minimum width, as a host opens it.
            viewport: { width: 1200, height: 720 },
          },
        },
      },
    ],
  },
});
