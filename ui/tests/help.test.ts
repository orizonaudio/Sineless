import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, test } from "vitest";

import { ChannelView, DisplayMode } from "../../protocol/src/enums.ts";
import { helpTexts, isHelpTopic } from "../src/view/help.ts";
import { readoutView } from "../src/view/screen-state.ts";

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
    // Not vacuous: both names are found in help.
    const texts = Object.values(helpTexts).map(({ text }) => text);
    expect(texts.some((text) => text.includes(multi ?? "?"))).toBe(true);
    expect(texts.some((text) => text.includes(compare ?? "?"))).toBe(true);
    const partial = Object.entries(helpTexts)
      .filter(
        ([, { text }]) =>
          text.includes(multi ?? "") && !text.includes(compare ?? ""),
      )
      .map(([topic]) => topic);
    expect(partial).toEqual([]);
  });

  // The help for what the readouts measure names the channel they read, on a stereo and on a mono
  // track, and no other: the trigger source picks only what the trigger reads.
  const channelName = (view: ChannelView) => {
    const key =
      Object.entries(ChannelView).find(([, value]) => value === view)?.[0] ??
      "";
    return key.charAt(0).toUpperCase() + key.slice(1);
  };
  test.each(["peak", "frequency", "readoutSource"] as const)(
    "%s names the channel the readouts measure",
    (topic) => {
      const named = Object.values(ChannelView)
        .map(channelName)
        .filter((name) =>
          new RegExp(`\\b${name}\\b`, "u").test(helpTexts[topic].text),
        );
      expect(named.sort()).toEqual(
        [...new Set([readoutView(2), readoutView(1)].map(channelName))].sort(),
      );
    },
  );

  test("have a title and full sentences", () => {
    for (const { title, text } of Object.values(helpTexts)) {
      expect(title.length).toBeGreaterThan(0);
      expect(text).toMatch(/^[A-Z⌘].*\.$/u);
    }
  });
});
