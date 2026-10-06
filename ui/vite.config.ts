import { defineConfig } from "vite";

export default defineConfig({
  build: {
    // WebKit shipped with macOS 13 (DESIGN C-2, C-5).
    target: "safari16",
    outDir: "../build/ui",
    emptyOutDir: true,
  },
});
