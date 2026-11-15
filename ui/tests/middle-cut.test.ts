// A long track name is cut in the middle to fit (#425): the reader keeps its start and its end,
// which tell similar tracks apart, and never sees half a character.
import { describe, expect, test } from "vitest";

import { ellipsis, middleCut } from "../src/view/middle-cut.ts";

const segmenter = new Intl.Segmenter(undefined, { granularity: "grapheme" });
const characters = (text: string) =>
  [...segmenter.segment(text)].map((part) => part.segment);
// A font in which every character is as wide as every other.
const fitsIn = (width: number) => (text: string) =>
  characters(text).length <= width;

const names = [
  "Drum Bus Parallel Crush Right",
  "Lead Vocal Double (Tuned, Printed)",
  "Kick",
  "Bass 2",
  // A family of four as one emoji, a flag, a skin tone and a letter with a combining accent.
  "Choir 👨‍👩‍👧‍👦 take 🇧🇷 with 👍🏽 and énd",
  "🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸",
];

describe("a name cut in the middle", () => {
  test.each(names)("%s is whole wherever it fits", (name) => {
    const length = characters(name).length;
    expect(middleCut(name, fitsIn(length))).toBe(name);
    expect(middleCut(name, fitsIn(length + 10))).toBe(name);
  });

  const cases = names.flatMap((name) =>
    Array.from({ length: characters(name).length - 1 }, (_, index) => ({
      name,
      width: index + 1,
    })),
  );
  test.each(cases)(
    "$name in $width characters fits, and keeps its start and its end whole",
    ({ name, width }) => {
      const cut = middleCut(name, fitsIn(width));
      expect(characters(cut).length).toBeLessThanOrEqual(width);
      const [head, tail, ...more] = cut.split(ellipsis);
      expect(more).toEqual([]);
      expect(name.startsWith(head ?? "")).toBe(true);
      expect(name.endsWith(tail ?? "")).toBe(true);
      // Whole characters only: each kept piece is made of the name's own characters.
      const whole = characters(name);
      const kept = [...characters(head ?? ""), ...characters(tail ?? "")];
      expect(whole.slice(0, characters(head ?? "").length)).toEqual(
        characters(head ?? ""),
      );
      expect(whole.slice(whole.length - characters(tail ?? "").length)).toEqual(
        characters(tail ?? ""),
      );
      expect(kept.length).toBeLessThan(whole.length);
    },
  );

  // Nothing longer would fit: the cut is not shorter than it has to be.
  test.each(cases.filter(({ width }) => width >= 4))(
    "$name in $width characters wastes no room it could fill",
    ({ name, width }) => {
      const cut = middleCut(name, fitsIn(width));
      // A space dropped beside the cut is the one character a cut may leave unused each side.
      expect(characters(cut).length).toBeGreaterThanOrEqual(width - 2);
    },
  );

  test("keeps both ends of two names that differ only at the end", () => {
    const left = middleCut("Drum Bus Parallel Crush Left", fitsIn(16));
    const right = middleCut("Drum Bus Parallel Crush Right", fitsIn(16));
    expect(left).not.toBe(right);
    expect(left.startsWith("Drum Bus")).toBe(true);
    expect(left.endsWith("Left")).toBe(true);
    expect(right.endsWith("Right")).toBe(true);
  });

  test("shows the ellipsis alone where not even one character fits beside it", () => {
    expect(middleCut("Bass", fitsIn(1))).toBe(ellipsis);
    expect(middleCut("Bass", fitsIn(0))).toBe(ellipsis);
  });
});
