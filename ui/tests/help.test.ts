import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, test } from "vitest";

import { DisplayMode } from "../../protocol/src/enums.ts";
import { helpTexts, isHelpTopic } from "../src/view/help.ts";

const ui = join(import.meta.dirname, "..");

function sourceFiles(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) =>
    entry.isDirectory()
      ? sourceFiles(join(directory, entry.name))
      : entry.name.endsWith(".ts")
        ? [join(directory, entry.name)]
        : [],
  );
}

const page = readFileSync(join(ui, "index.html"), "utf8");
const code = sourceFiles(join(ui, "src"))
  .filter((file) => !file.endsWith("help.ts"))
  .map((file) => readFileSync(file, "utf8"))
  .join("\n");

describe("help texts", () => {
  test("exist for every topic the page names", () => {
    const named = [...page.matchAll(/data-help="([^"]+)"/gu)].map(
      (match) => match[1] ?? "",
    );
    expect(named.length).toBeGreaterThan(20);
    expect(named.filter((topic) => !isHelpTopic(topic))).toEqual([]);
  });

  test("are all used by the page or the code", () => {
    const unused = Object.keys(helpTexts).filter(
      (topic) =>
        !page.includes(`data-help="${topic}"`) && !code.includes(`"${topic}"`),
    );
    expect(unused).toEqual([]);
  });

  // Compare shows what Multi shows, each track's Mid with no views or trigger source of its own
  // (multiMode in app.ts), and more: help that says something of Multi says it of Compare too.
  test("that say something of Multi mode say it of Compare mode too", () => {
    const [, multi, compare] = Object.keys(DisplayMode).map(
      (mode) => mode.charAt(0).toUpperCase() + mode.slice(1),
    );
    const partial = Object.entries(helpTexts)
      .filter(
        ([, { text }]) =>
          text.includes(multi ?? "") && !text.includes(compare ?? ""),
      )
      .map(([topic]) => topic);
    expect(partial).toEqual([]);
  });

  test("have a title and full sentences", () => {
    for (const { title, text } of Object.values(helpTexts)) {
      expect(title.length).toBeGreaterThan(0);
      expect(text).toMatch(/^[A-Z⌘].*\.$/u);
    }
  });
});
