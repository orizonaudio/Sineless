// The hover readout sits beside the pointer, below and right of it, flipped where the screen ends,
// and at whole pixels: the pointer comes in fractions of one, and text placed at a fraction snaps
// to the pixel grid apart from its box, stepping a pixel as the hand moves.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, startApp } from "./harness.ts";

// The readout's gap to the pointer (CSS px), as the page places it.
const offset = 14;

describe("the hover readout", () => {
  test("sits beside the pointer at whole pixels, flipped where the screen ends", async () => {
    await startApp();
    await userEvent.click(byId("hover-toggle"));
    await settle();
    const trace = byId("trace").getBoundingClientRect();
    const screen = byId("screen");
    const node = byId("hover-readout");
    // From the trace's left and top, as fractions of a pixel would come; the last two near the
    // right and bottom edges, where the readout flips to the pointer's other side.
    for (const [x, y] of [
      [120.37, 80.61],
      [240.5, 81.25],
      [361.93, 79.4],
      [trace.width - 20.3, trace.height - 10.6],
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
        expect(Number.isFinite(pixels), edge).toBe(true);
        expect(pixels, edge).toBe(Math.round(pixels));
      }
      // The pointer in the screen's own pixels, as the page reads it.
      const px = (x / trace.width) * screen.clientWidth;
      const py = (y / trace.height) * screen.clientHeight;
      const right = px + offset + node.offsetWidth <= screen.clientWidth;
      const below = py + offset + node.offsetHeight <= screen.clientHeight;
      const left = right ? px + offset : px - offset - node.offsetWidth;
      const top = below ? py + offset : py - offset - node.offsetHeight;
      expect(node.offsetLeft).toBeCloseTo(Math.max(0, left), -0.5);
      expect(node.offsetTop).toBeCloseTo(Math.max(0, top), -0.5);
    }
  });
});
