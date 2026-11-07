// The hover readout sits at whole pixels: the pointer comes in fractions of one, and text placed at
// a fraction snaps to the pixel grid apart from its box, stepping a pixel as the hand moves.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, startApp } from "./harness.ts";

describe("the hover readout", () => {
  test("is placed at whole pixels for a pointer between them", async () => {
    await startApp();
    await userEvent.click(byId("hover-toggle"));
    await settle();
    const trace = byId("trace").getBoundingClientRect();
    const node = byId("hover-readout");
    for (const [x, y] of [
      [120.37, 80.61],
      [240.5, 81.25],
      [361.93, 79.4],
    ] as const) {
      byId("trace").dispatchEvent(
        new PointerEvent("pointermove", {
          bubbles: true,
          buttons: 0,
          clientX: trace.left + x,
          clientY: trace.top + y,
        }),
      );
      expect(node.hidden).toBe(false);
      for (const edge of [node.style.left, node.style.top]) {
        const pixels = Number.parseFloat(edge);
        expect(pixels, edge).toBe(Math.round(pixels));
      }
    }
  });
});
