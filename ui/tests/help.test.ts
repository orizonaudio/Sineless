import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, test } from "vitest";

import { DisplayMode } from "../../protocol/src/enums.ts";
import { maxLanes } from "../../protocol/src/limits.ts";
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

  // Compare is a switch in Multi, not a display mode: help that called it one would send the user
  // looking for a button that is not there.
  test("never call Compare a display mode", () => {
    const modes = Object.keys(DisplayMode).map(
      (mode) => mode.charAt(0).toUpperCase() + mode.slice(1),
    );
    expect(modes).not.toContain("Compare");
    const calling = Object.entries(helpTexts)
      .filter(([, { text }]) =>
        /Compare modes?\b|Multi and Compare|Multi or Compare/u.test(text),
      )
      .map(([topic]) => topic);
    expect(calling).toEqual([]);
  });

  // The switch's own help says where it applies, that it is in beta, and how many tracks it leaves
  // room for beside the sum.
  test("of the Compare switch say it is in beta and leaves a lane for the sum", () => {
    const { text } = helpTexts.compare;
    expect(text).toContain("beta");
    expect(text).toContain("Multi");
    expect(text).toContain(String(maxLanes - 1));
  });

  // The readouts' channel is chosen (and named beside them), so no help says they measure one
  // fixed channel; the trigger source picks only what the trigger reads.
  test.each(["peak", "rms", "frequency", "triggerSource"] as const)(
    "%s does not tie the readouts to one channel",
    (topic) => {
      expect(helpTexts[topic].text).not.toMatch(
        /(?:readouts measure|of) (?:this track's )?(?:the )?(?:Mid|Left|Right|Side)\b/u,
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
