// A build without the brand (SINELESS_BRANDING=OFF, cmake/Branding.cmake) carries none of the
// logo's files, which have terms of their own (ui/src/brand/README.md), and none of the names; the
// branded build does.
import { mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build } from "vite";
import { afterAll, describe, expect, test } from "vitest";

const ui = join(import.meta.dirname, "..");
const brandFiles = readdirSync(join(ui, "src", "brand"))
  .filter((name) => name.endsWith(".svg"))
  .map((name) => readFileSync(join(ui, "src", "brand", name), "utf8"));
// What identifies each brand file in a page: the path data of its first shape.
const brandMarks = brandFiles.map(
  (svg) => /\sd="([^"]{40})/u.exec(svg)?.[1] ?? svg,
);

const outputs: string[] = [];
afterAll(() => {
  for (const output of outputs) {
    rmSync(output, { recursive: true, force: true });
  }
});

// The page's files as one text, built with the brand on or off.
async function builtPage(branding: "ON" | "OFF"): Promise<string> {
  const outDir = mkdtempSync(join(tmpdir(), "sineless-page-"));
  outputs.push(outDir);
  const previous = process.env["SINELESS_BRANDING"];
  process.env["SINELESS_BRANDING"] = branding;
  try {
    await build({
      configFile: join(ui, "vite.config.ts"),
      root: ui,
      logLevel: "silent",
      build: { outDir, emptyOutDir: true },
    });
  } finally {
    if (previous === undefined) {
      delete process.env["SINELESS_BRANDING"];
    } else {
      process.env["SINELESS_BRANDING"] = previous;
    }
  }
  return readdirSync(outDir, { recursive: true, withFileTypes: true })
    .filter((entry) => entry.isFile())
    .map((entry) => readFileSync(join(entry.parentPath, entry.name), "latin1"))
    .join("\n");
}

describe("the page's brand", () => {
  test("has files to find", () => {
    expect(brandMarks.length).toBeGreaterThan(0);
  });

  test("is in a branded build and left out of one without the brand", async () => {
    const branded = await builtPage("ON");
    const unbranded = await builtPage("OFF");
    for (const mark of brandMarks) {
      expect(branded).toContain(mark);
      expect(unbranded).not.toContain(mark);
    }
    expect(branded).toContain("Sineless");
    for (const name of ["Sineless", "Orizon"]) {
      expect(unbranded).not.toContain(name);
    }
  }, 60000);
});
