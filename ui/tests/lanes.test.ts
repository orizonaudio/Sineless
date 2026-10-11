import { describe, expect, test } from "vitest";

import { cssColour, fallbackPalette, laneStyles } from "../src/view/lanes.ts";

describe("lane styles", () => {
  test("use the host's names and colours", () => {
    const kick = {
      id: 1,
      name: "Kick",
      colour: 0xe0533d,
      own: true,
      compared: true,
    };
    expect(laneStyles([kick])).toEqual([kick]);
  });

  test("fall back to Track n and the palette by lane position", () => {
    const styles = laneStyles([
      { id: 1, name: "Kick", colour: 0xe0533d, own: false, compared: false },
      { id: 2, name: "", colour: null, own: true, compared: false },
    ]);
    expect(styles[1]).toEqual({
      id: 2,
      name: "Track 2",
      colour: fallbackPalette[1],
      own: true,
      compared: false,
    });
  });

  test("write colours for CSS", () => {
    expect(cssColour(0x0a0b0c)).toBe("#0a0b0c");
  });

  test("has a distinct colour for every lane, none the sum lane's", () => {
    expect(fallbackPalette).toHaveLength(16);
    expect(new Set(fallbackPalette).size).toBe(16);
    expect(fallbackPalette).not.toContain(0xe6e2d8);
  });
});
