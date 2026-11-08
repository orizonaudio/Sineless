import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, test } from "vitest";

import { DisplayMode, enums } from "../../protocol/src/enums.ts";
import { maxLanes } from "../../protocol/src/limits.ts";
import { levelLines } from "../src/view/axis-labels.ts";
import { settings } from "../../protocol/src/settings.ts";
import { choiceLabels } from "../src/view/choice-labels.ts";
import {
  choiceHelp,
  helpTexts,
  helpTextsFor,
  helpWords,
  type HelpTopic,
  isHelpTopic,
  leadWordBudget,
  topicWordBudget,
  triggerModeHelp,
} from "../src/view/help.ts";
import { beatSpanLabels, parseBeatSpan } from "../src/view/musical-grid.ts";

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

const wordCount = (text: string) => text.split(/\s+/u).filter(Boolean).length;

// Help names the keys as the platform does, whichever platform runs the tests: the page also runs
// in WebView2 on Windows.
const platforms = [
  {
    platform: "macOS",
    texts: helpTextsFor(true),
    keys: ["⌘", "⌥"],
    others: ["Ctrl", "Alt"],
  },
  {
    platform: "Windows",
    texts: helpTextsFor(false),
    keys: ["Ctrl", "Alt"],
    others: ["⌘", "⌥"],
  },
] as const;

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
        !page.includes(`data-help="${topic}"`) &&
        !code.includes(`"${topic}"`) &&
        !Object.values(triggerModeHelp).includes(topic as HelpTopic),
    );
    expect(unused).toEqual([]);
  });

  // A user who opens a setting's choices finds each one explained under the name the control
  // shows: a time base, mode, layout or slope added without its help fails here.
  const enumSettings = Object.entries(settings).flatMap(([name, setting]) =>
    setting.kind === "enum" ? [{ name, values: setting.values }] : [],
  );
  test.each(enumSettings)(
    "explain each value of $name, as the control names it, or say why not",
    ({ name, values }) => {
      const entry = (choiceHelp as Record<string, unknown>)[name];
      expect(entry, name).toBeDefined();
      if (
        entry !== undefined &&
        entry !== null &&
        typeof entry === "object" &&
        "exempt" in entry
      ) {
        expect(String(entry.exempt).length).toBeGreaterThan(10);
        return;
      }
      const { topic, labels: labelsName = name } = entry as {
        topic: keyof typeof helpTexts;
        labels?: string;
      };
      const labels = (choiceLabels as Record<string, Record<string, string>>)[
        labelsName
      ];
      expect(labels, `${name} has labels`).toBeDefined();
      expect(Object.keys(labels ?? {}).sort()).toEqual(
        Object.keys(enums[values]).sort(),
      );
      const help = helpTexts[topic];
      const choices = "choices" in help ? help.choices : [];
      expect(choices.map((choice) => choice.name)).toEqual(
        Object.values(labels ?? {}),
      );
    },
  );

  // Each trigger mode button explains its own mode, titled as the button reads.
  test.each(Object.entries(choiceLabels.triggerMode))(
    "explain the %s trigger mode on its own button",
    (mode, label) => {
      const topic = triggerModeHelp[mode as keyof typeof triggerModeHelp];
      expect(helpTexts[topic].title).toBe(label);
    },
  );

  describe.each(platforms)("for $platform", ({ texts, keys, others }) => {
    const topics = Object.entries(texts);

    // A user resting on a control reads one short line first and decides from it whether to read
    // on; past the budget a tip stops being quick to read.
    test.each(topics)(
      "%s has a short lead and stays within its budget",
      (_, text) => {
        expect(wordCount(text.lead)).toBeGreaterThan(0);
        expect(wordCount(text.lead)).toBeLessThanOrEqual(leadWordBudget);
        expect(wordCount(helpWords(text))).toBeLessThanOrEqual(topicWordBudget);
      },
    );

    // The lead and notes read as sentences; a choice's or a gesture's text reads as a line of a
    // list, without a full stop.
    test.each(topics)("%s writes sentences and list lines apart", (_, text) => {
      const sentences = [text.lead, ...("notes" in text ? text.notes : [])];
      for (const sentence of sentences) {
        expect(sentence).toMatch(/^[A-Z⌘⌥—].*\.$/u);
      }
      const lines = [
        ...("choices" in text
          ? text.choices.flatMap((c) => [c.name, c.text])
          : []),
        ...("actions" in text
          ? text.actions.flatMap((a) => [a.gesture, a.effect])
          : []),
      ];
      for (const line of lines) {
        expect(line.length).toBeGreaterThan(0);
        expect(line).not.toMatch(/\.$/u);
      }
    });

    test("name the platform's keys and never the other's", () => {
      const words = topics.map(([, text]) => helpWords(text)).join("\n");
      for (const key of keys) {
        expect(words).toContain(key);
      }
      for (const key of others) {
        expect(words).not.toMatch(
          new RegExp(`(?<![\\p{L}])${key}(?![\\p{L}])`, "u"),
        );
      }
    });

    // Compare is a switch in Multi, not a display mode: help that called it one would send the
    // user looking for a button that is not there.
    test("never call Compare a display mode", () => {
      const modes = Object.keys(DisplayMode).map(
        (mode) => mode.charAt(0).toUpperCase() + mode.slice(1),
      );
      expect(modes).not.toContain("Compare");
      const calling = topics
        .filter(([, text]) =>
          /Compare modes?\b|Multi and Compare|Multi or Compare/u.test(
            helpWords(text),
          ),
        )
        .map(([topic]) => topic);
      expect(calling).toEqual([]);
    });

    // The switch's own help says where it applies, that it is in beta, and how many tracks it
    // leaves room for beside the sum.
    test("of the Compare switch say it is in beta and leaves a lane for the sum", () => {
      const words = helpWords(texts.compare);
      expect(words).toContain("beta");
      expect(words).toContain("Multi");
      expect(words).toContain(String(maxLanes - 1));
    });

    // The Meter's channel is chosen (and named) in the Views popover's Measure row, so no help says it
    // measures one fixed channel; the trigger source picks only what the trigger reads.
    test.each(["meter", "triggerSource"] as const)(
      "%s does not tie the Meter to one channel",
      (topic) => {
        expect(helpWords(texts[topic])).not.toMatch(
          /(?:readouts measure|of) (?:this track's )?(?:the )?(?:Mid|Left|Right|Side)\b/u,
        );
      },
    );

    // The channel read is chosen in the Views popover's Measure row: help that sends the user to it
    // names that row, and none names the Channel button the status bar once held.
    test.each(Object.keys(texts) as (keyof typeof texts)[])(
      "%s names no Channel button",
      (topic) => {
        expect(helpWords(texts[topic])).not.toContain("Channel button");
      },
    );
    test.each(["meter", "triggerSource"] as const)(
      "%s says the channel read is chosen in the Measure row",
      (topic) => {
        expect(helpWords(texts[topic])).toContain("Measure");
      },
    );

    // The span's help names its range, the shortest and the longest span, and offers numbers to
    // type that set the span it says they do.
    test("of the beat span name its range and numbers that type the spans they name", () => {
      const words = helpWords(texts.beatSpan);
      expect(words).toContain(beatSpanLabels[0]);
      expect(words).toContain(beatSpanLabels.at(-1) ?? "");
      const offered = [
        ...words.matchAll(/(\S+) for ([^,.;\n]+?)(?=[,.;\n]|$)/gu),
      ];
      expect(offered.length).toBeGreaterThan(0);
      for (const [, typed = "", named = ""] of offered) {
        expect(typed).not.toBe(named);
        expect(parseBeatSpan(typed), typed).toBe(
          beatSpanLabels.indexOf(named as (typeof beatSpanLabels)[number]),
        );
      }
    });
  });
});

describe("the grid's Level help", () => {
  // The spacing a tall lane's lines stand at, at a Level step: the gap between its first two lines,
  // or between Fine's minor lines.
  const spacing = (density: "coarse" | "medium" | "fine", strong: boolean) => {
    const steps = levelLines(1000, density)
      .filter((line) => line.strong === strong || density !== "fine")
      .map((line) => line.step);
    return (steps[1] ?? 0) - (steps[0] ?? 0);
  };

  test("names the spacing the grid draws at Coarse, Medium and Fine", () => {
    const text = helpWords(helpTexts.gridLevel);
    expect(text).toContain(`every ${String(spacing("coarse", false))} dB`);
    expect(text).toContain(`every ${String(spacing("medium", false))} dB`);
    const fineMinor = levelLines(1000, "fine").filter((line) => !line.strong);
    const minor = (fineMinor[1]?.step ?? 0) - (fineMinor[0]?.step ?? 0);
    expect(text).toContain(`with ${String(minor)} dB lines between them`);
  });
});
