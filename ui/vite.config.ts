import { defineConfig } from "vite";

export default defineConfig({
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
