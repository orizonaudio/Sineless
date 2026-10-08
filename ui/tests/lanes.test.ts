import { describe, expect, test } from "vitest";

import { cssColour, fallbackPalette, laneStyles } from "../src/view/lanes.ts";

describe("lane styles", () => {
  test("use the host's names and colours", () => {
    expect(
      laneStyles([{ id: 1, name: "Kick", colour: 0xe0533d, own: true }]),
    ).toEqual([{ id: 1, name: "Kick", colour: 0xe0533d, own: true }]);
  });

  test("fall back to Track n and the palette by lane position", () => {
    const styles = laneStyles([
      { id: 1, name: "Kick", colour: 0xe0533d, own: false },
      { id: 2, name: "", colour: null, own: true },
    ]);
    expect(styles[1]).toEqual({
      id: 2,
      name: "Track 2",
      colour: fallbackPalette[1],
      own: true,
    });
  });

  test("write colours for CSS", () => {
    expect(cssColour(0x0a0b0c)).toBe("#0a0b0c");
  });
});
