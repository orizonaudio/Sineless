// The hover on the real engine's frame: a 1 kHz sine at 44.1 kHz over 10 ms, 441 samples, fewer
// than the page's columns, so the frame carries raw samples and the hover reads the cycle exactly,
// timed at the rate the frame carries.
import { describe, expect, test } from "vitest";

import { formatFrequency, formatSeconds } from "../../src/view/format.ts";
import { beforeApp, byId, settle, startRecorded } from "./harness.ts";

const hertz = 1000;

// Rests the pointer, no button held, at (`x`, `y`), fractions of the trace.
function hoverAt(x: number, y: number): void {
  const box = byId("trace").getBoundingClientRect();
  byId("trace").dispatchEvent(
    new PointerEvent("pointermove", {
      bubbles: true,
      buttons: 0,
      clientX: box.left + x * box.width,
      clientY: box.top + y * box.height,
    }),
  );
}

describe("the hover on a recorded 1 kHz sine at 44.1 kHz", () => {
  beforeApp(async () => {
    await startRecorded("SineHalf1k_44k1");
  });

  test("reads one cycle as a millisecond and 1 kHz", async () => {
    await settle();
    // The upper lane, L (stacked by default), half way across.
    hoverAt(0.5, 0.25);
    // The readout's rows by label.
    const row = (label: string) =>
      [...byId("hover-readout").querySelectorAll(".cursor-row")]
        .find((line) => line.querySelector("span")?.textContent === label)
        ?.querySelector("b")?.textContent;
    await expect.poll(() => row("f")).toBe(`${formatFrequency(hertz)} Hz`);
    expect(row("Cycle")).toBe(formatSeconds(1 / hertz));
  });
});
