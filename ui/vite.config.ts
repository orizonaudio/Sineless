import { fileURLToPath } from "node:url";

import { defineConfig } from "vite";

// The product's name and logo come from ui/src/brand/, which has terms of its own; a build of a
// modified version leaves it out (SINELESS_BRANDING=OFF, set by CMake's option of that name) and
// takes the neutral ui/src/unbranded/ instead.
const branded = process.env["SINELESS_BRANDING"] !== "OFF";

export default defineConfig({
  resolve: {
    alias: {
      "@brand": fileURLToPath(
        new URL(branded ? "./src/brand" : "./src/unbranded", import.meta.url),
      ),
    },
  },
  build: {
    // WebKit shipped with macOS 13.1, the minimum (Safari 16.2: color-mix()).
    target: "safari16.2",
    outDir: "../build/ui",
    emptyOutDir: true,
    // Every asset its own file: the editor's Content-Security-Policy (editor/src/EmbeddedAssets.h)
    // allows fonts and styles from the page's origin, not inlined data: URLs.
    assetsInlineLimit: 0,
  },
});
