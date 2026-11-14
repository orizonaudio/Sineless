import { readFileSync } from "node:fs";

import { describe, expect, test } from "vitest";

import {
  contrast,
  cssColour,
  fallbackPalette,
  laneName,
  laneStyles,
  screenColour,
  shade,
  shadeContrast,
  sumLaneColour,
} from "../src/view/lanes.ts";

describe("lane styles", () => {
  test("use the host's names and colours", () => {
    const kick = {
      id: 1,
      name: "Kick",
      colour: 0xe0533d,
      own: true,
      compared: true,
      instance: 0,
    };
    expect(laneStyles([kick])).toEqual([kick]);
  });

  test("fall back to Track n and the palette by lane position", () => {
    const styles = laneStyles([
      {
        id: 1,
        name: "Kick",
        colour: 0xe0533d,
        own: false,
        compared: false,
        instance: 0,
      },
      {
        id: 2,
        name: "",
        colour: null,
        own: true,
        compared: false,
        instance: 0,
      },
    ]);
    expect(styles[1]).toEqual({
      id: 2,
      name: "Track 2",
      colour: fallbackPalette[1],
      own: true,
      compared: false,
      instance: 0,
    });
  });

  test("write colours for CSS", () => {
    expect(cssColour(0x0a0b0c)).toBe("#0a0b0c");
  });

  test("has a distinct colour for every lane, none the sum lane's", () => {
    expect(fallbackPalette).toHaveLength(16);
    expect(new Set(fallbackPalette).size).toBe(16);
    expect(fallbackPalette).not.toContain(sumLaneColour);
  });
});

// Two instances on one track, one before an effect and one after: the host gives both the
// track's name and colour, and the lanes must still be told apart.
describe("several instances on one track", () => {
  const bass = (id: number, instance: number, colour: number | null) => ({
    id,
    name: "Bass 2",
    colour,
    own: false,
    compared: false,
    instance,
  });

  test("the first keeps the track's colour and the next take shades of it", () => {
    const styles = laneStyles([
      bass(1, 1, 0x4f8fe6),
      bass(2, 2, 0x4f8fe6),
      bass(3, 3, 0x4f8fe6),
    ]);
    expect(styles[0]?.colour).toBe(0x4f8fe6);
    expect(
      new Set(styles.map((style) => style.colour)).size,
      "each lane its own colour",
    ).toBe(3);
  });

  test("lanes without a host colour keep the palette, which already tells them apart", () => {
    const styles = laneStyles([bass(1, 1, null), bass(2, 2, null)]);
    expect(styles.map((style) => style.colour)).toEqual([
      fallbackPalette[0],
      fallbackPalette[1],
    ]);
  });

  test("name each lane with its number, and say it is an instance", () => {
    expect(laneName(bass(2, 1, null))).toEqual({
      name: "Bass 2",
      number: "1",
      spoken: ", instance 1",
    });
  });

  test("a track's only instance keeps its plain name", () => {
    expect(laneName(bass(1, 0, null))).toEqual({
      name: "Bass 2",
      number: undefined,
      spoken: undefined,
    });
  });

  // Host colours across the range: the palette's own, and a dark, a light and a saturated one.
  const hostColours = [
    ...fallbackPalette,
    0x1c2a6b,
    0xf2f2ee,
    0xff0000,
    0x4f8fe6,
  ];
  describe.each(hostColours.map((colour) => [cssColour(colour), colour]))(
    "shades of %s",
    (_, colour) => {
      const shades = [2, 3, 4].map((instance) => shade(colour, instance));

      test("differ from the colour and from each other", () => {
        expect(new Set([colour, ...shades]).size).toBe(4);
      });

      test("stay legible as traces on the screen", () => {
        for (const shaded of shades) {
          expect(
            contrast(shaded, screenColour),
            cssColour(shaded),
          ).toBeGreaterThanOrEqual(shadeContrast);
        }
      });
    },
  );

  test("the screen colour the shades are judged against is the page's", () => {
    const tokens = readFileSync(
      new URL("../src/tokens.css", import.meta.url),
      "utf8",
    );
    expect(tokens).toContain(`--sl-screen: ${cssColour(screenColour)};`);
  });
});
