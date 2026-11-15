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
      instance: 0,
    };
    expect(laneStyles([kick])).toEqual([kick]);
  });

  test("fall back to Track n and the palette by lane position, before the list of tracks comes", () => {
    const styles = laneStyles([
      {
        id: 1,
        name: "Kick",
        colour: 0xe0533d,
        own: false,
        instance: 0,
      },
      {
        id: 2,
        name: "",
        colour: null,
        own: true,
        instance: 0,
      },
    ]);
    expect(styles[1]).toEqual({
      id: 2,
      name: "Track 2",
      colour: fallbackPalette[1],
      own: true,
      instance: 0,
    });
  });

  // The same track reads alike in the Views popover's list and on the screen, whichever tracks
  // are shown: a track the host gave no name or colour takes those of its place in the list.
  test("name and colour an unnamed track by its place among every track, not among those shown", () => {
    const unnamed = {
      id: 7,
      name: "",
      colour: null,
      own: false,
      instance: 0,
    };
    const places = new Map([
      [1, 0],
      [4, 1],
      [7, 2],
    ]);
    // Shown with this track alone, it is still the host's third track.
    const [, shown] = laneStyles(
      [{ ...unnamed, id: 1, name: "Kick", own: true }, unnamed],
      places,
    );
    expect(shown?.name).toBe("Track 3");
    expect(shown?.colour).toBe(fallbackPalette[2]);
    // The list itself: every track, each at its place.
    const listed = laneStyles(
      [
        { ...unnamed, id: 1, name: "Kick", own: true },
        { ...unnamed, id: 4 },
        unnamed,
      ],
      places,
    );
    expect(listed[2]).toEqual(shown);
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

  // Host colours across the range: the palette's own, a dark, a light and a saturated one, and
  // black, which has only lighter shades to take.
  const hostColours = [
    ...fallbackPalette,
    0x1c2a6b,
    0xf2f2ee,
    0xff0000,
    0x4f8fe6,
    0x000000,
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

      test("are colours the page can draw", () => {
        for (const shaded of shades) {
          expect(cssColour(shaded)).toMatch(/^#[0-9a-f]{6}$/);
        }
      });
    },
  );

  // A colour's hue and saturation as OKLab's a and b, by the route CSS Color 4 gives (sRGB to
  // CIE XYZ, then Björn Ottosson's two matrices), so a shade can be checked against the standard
  // and not against the page's own arithmetic.
  function hueAndSaturation(colour: number): [number, number] {
    const [r, g, b] = [colour >> 16, colour >> 8, colour].map((channel) => {
      const c = (channel & 0xff) / 255;
      return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
    }) as [number, number, number];
    const x = 0.4123907993 * r + 0.3575843394 * g + 0.1804807884 * b;
    const y = 0.2126390059 * r + 0.7151686788 * g + 0.0721923154 * b;
    const z = 0.0193308187 * r + 0.1191947798 * g + 0.9505321522 * b;
    const l = Math.cbrt(0.8189330101 * x + 0.3618667424 * y - 0.1288597137 * z);
    const m = Math.cbrt(0.0329845436 * x + 0.9293118715 * y + 0.0361456387 * z);
    const s = Math.cbrt(0.0482003018 * x + 0.2643662691 * y + 0.633851707 * z);
    return [
      1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
      0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
    ];
  }

  test("a shade the screen can show in full keeps the track's hue and saturation", () => {
    let shown = 0;
    for (const colour of hostColours) {
      const [a, b] = hueAndSaturation(colour);
      for (const instance of [2, 3, 4]) {
        const shaded = shade(colour, instance);
        const clipped = [shaded >> 16, shaded >> 8, shaded].some(
          (channel) => (channel & 0xff) === 0 || (channel & 0xff) === 0xff,
        );
        if (clipped) {
          // Past what the screen can show, a shade is clipped to it channel by channel, which
          // moves its hue and saturation: only shades shown in full are held to the track's.
          continue;
        }
        shown += 1;
        const [shadedA, shadedB] = hueAndSaturation(shaded);
        // Within what rounding to 8 bits a channel moves a colour.
        expect(
          Math.hypot(shadedA - a, shadedB - b),
          `${cssColour(colour)} as ${cssColour(shaded)}`,
        ).toBeLessThan(0.004);
      }
    }
    expect(shown, "most colours have such a shade").toBeGreaterThan(
      hostColours.length,
    );
  });

  // The contrast ratios WCAG's definition gives for colours anyone can check: the bar the shades
  // are held to means what it says only if the ratio is WCAG's.
  test.each([
    ["black on white", 0x000000, 0xffffff, 21],
    ["blue on black", 0x0000ff, 0x000000, 2.44],
    ["red on white", 0xff0000, 0xffffff, 4],
    ["the grey that just passes on white", 0x767676, 0xffffff, 4.54],
    ["the grey that just fails on white", 0x777777, 0xffffff, 4.48],
  ])("contrast reads %s as WCAG does", (_, first, second, ratio) => {
    expect(contrast(first, second)).toBeCloseTo(ratio, 2);
    expect(contrast(second, first)).toBeCloseTo(ratio, 2);
  });

  test("the screen colour the shades are judged against is the page's", () => {
    const tokens = readFileSync(
      new URL("../src/tokens.css", import.meta.url),
      "utf8",
    );
    expect(tokens).toContain(`--sl-screen: ${cssColour(screenColour)};`);
  });
});
