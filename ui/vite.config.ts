import { defineConfig } from "vite";

export default defineConfig({
  build: {
    // WebKit shipped with macOS 13.1, the minimum (Safari 16.2: color-mix()).
    target: "safari16.2",
    outDir: "../build/ui",
    emptyOutDir: true,
  },
});
