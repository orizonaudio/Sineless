// The page at the highest sample rate a host runs, 384 kHz: offsets and cursor readouts follow
// the rate the engine reports, not 48 kHz.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { parseQuantity } from "../../src/view/format.ts";
import { laneReadout } from "./compare-readout.ts";
import {
  beforeApp,
  byId,
  choice,
  cursorRow,
  press,
  settle,
  showDisplay,
  startApp,
} from "./harness.ts";

describe("at 384 kHz", () => {
  beforeApp(async () => {
    await startApp({ sampleRate: 384000 });
  });

  test("reads Kick 2's offset in milliseconds, as at any rate", async () => {
    await press(choice("time-base", "Trig"));
    await showDisplay("Compare");
    await settle();
    await settle();
    // 499 samples at 384 kHz.
    await expect.poll(() => laneReadout("Kick 2")).toBe("+1.3 ms Ø · −0.93");
  });

  test("counts the samples between time cursors at the reported rate", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("display-mode", "Single"));
    if (byId("time-cursors").getAttribute("aria-pressed") !== "true") {
      await userEvent.click(byId("time-cursors"));
    }
    await settle();
    const text = () => byId("cursor-readout").textContent;
    await expect.poll(text).toMatch(/samples/u);
    // "Δt 500 ms" and "192000.0 samples": samples over seconds is the rate, to Δt's three
    // significant digits.
    const seconds = parseQuantity(cursorRow("Δt"), "time");
    const samples = Number(cursorRow("Δs").split(" ")[0]);
    expect(seconds).toBeGreaterThan(0);
    expect(samples / (seconds ?? NaN)).toBeCloseTo(384000, -3);
  });
});
