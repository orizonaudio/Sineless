import { describe, expect, test } from "vitest";

import { cssColour, fallbackPalette, laneStyles } from "../src/view/lanes.ts";

describe("lane styles", () => {
  test("use the host's names and colours", () => {
    expect(laneStyles([{ name: "Kick", colour: 0xe0533d, own: true }])).toEqual(
      [{ name: "Kick", colour: 0xe0533d, own: true }],
    );
  });

  test("fall back to Track n and the palette by lane position", () => {
    const styles = laneStyles([
      { name: "Kick", colour: 0xe0533d, own: false },
      { name: "", colour: null, own: true },
    ]);
    expect(styles[1]).toEqual({
      name: "Track 2",
      colour: fallbackPalette[1],
      own: true,
    });
  });

  test("write colours for CSS", () => {
    expect(cssColour(0x0a0b0c)).toBe("#0a0b0c");
  });
});
