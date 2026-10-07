import { defineConfig } from "vite";

export default defineConfig({
  build: {
    // WebKit shipped with macOS 13.
    target: "safari16",
    outDir: "../build/ui",
    emptyOutDir: true,
  },
});
