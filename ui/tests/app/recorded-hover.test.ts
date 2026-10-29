// The hover on the real engine's frame: a 1 kHz sine at 44.1 kHz over 10 ms, 441 samples, fewer
// than the page's columns, so the frame carries raw samples and the hover reads the cycle exactly,
// timed at the rate the frame carries.
import { beforeAll, describe, expect, test } from "vitest";

import {
  formatFrequency,
  formatSamples,
  formatSeconds,
} from "../../src/view/format.ts";
import { byId, settle, startRecorded } from "./harness.ts";

const hertz = 1000;
const rate = 44100;

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
  beforeAll(async () => {
    await startRecorded("SineHalf1k_44k1");
  });

  test("reads one cycle as a millisecond, its samples at the frame's rate and 1 kHz", async () => {
    await settle();
    // The upper lane, L (stacked by default), half way across.
    hoverAt(0.5, 0.25);
    const text = () => byId("hover-readout").textContent;
    await expect.poll(text).toContain(" Hz");
    expect(text()).toContain(
      `${formatSeconds(1 / hertz)} · ${formatSamples(rate / hertz)}`,
    );
    expect(text()).toContain(`${formatFrequency(hertz)} Hz`);
  });
});
