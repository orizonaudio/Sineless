// The hover readout sits beside the pointer, below and right of it, flipped where the screen ends,
// and at whole pixels: the pointer comes in fractions of one, and text placed at a fraction snaps
// to the pixel grid apart from its box, stepping a pixel as the hand moves.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, startApp } from "./harness.ts";

// How near the pointer the readout's nearest edge must be to count as beside it (CSS px): a few
// lines of text, far less than the screen, so a readout left in a corner does not pass.
const beside = 40;

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
      const box = {
        left: node.offsetLeft,
        top: node.offsetTop,
        right: node.offsetLeft + node.offsetWidth,
        bottom: node.offsetTop + node.offsetHeight,
      };
      // Inside the screen, off the pointer, and near it.
      expect(box.left).toBeGreaterThanOrEqual(0);
      expect(box.top).toBeGreaterThanOrEqual(0);
      expect(box.right).toBeLessThanOrEqual(screen.clientWidth);
      expect(box.bottom).toBeLessThanOrEqual(screen.clientHeight);
      const covers =
        px > box.left && px < box.right && py > box.top && py < box.bottom;
      expect(covers).toBe(false);
      expect(
        Math.min(Math.abs(box.left - px), Math.abs(box.right - px)),
      ).toBeLessThan(beside);
      expect(
        Math.min(Math.abs(box.top - py), Math.abs(box.bottom - py)),
      ).toBeLessThan(beside);
      // Below and right of the pointer, or on its other side where the screen ends.
      const nearEdge = x > trace.width / 2 && y > trace.height / 2;
      expect(box.left > px).toBe(!nearEdge);
      expect(box.top > py).toBe(!nearEdge);
    }
  });
});
