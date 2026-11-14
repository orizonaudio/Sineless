// Compare showing the difference with Align on, on the real engine's frames: this track is noise
// after an "effect" that delays it by 46 ms, and the other track is the noise before it, flipped
// and at half its level. The engine lines the other up in time and polarity and says what it
// took out; the level is left, so the difference is the half this track has more, 6 dB below it.
import { describe, expect, test } from "vitest";

import { laneReadout } from "./compare-readout.ts";
import { beforeApp, byId, settle, startRecorded } from "./harness.ts";

describe("a recorded Compare showing the difference of lined-up tracks", () => {
  beforeApp(async () => {
    await startRecorded("CompareAligned_48k");
  });

  test("shows what Align took out, and the level change it left", async () => {
    await settle();
    const last = () => {
      const labels = byId("lane-labels").querySelectorAll(".lane-label > span");
      return labels[labels.length - 1];
    };
    // The other track is 46 ms early (2208 samples at 48 kHz) and flipped.
    await expect.poll(() => laneReadout("Before")).toBe("−46.0 ms · Ø");
    expect(last()?.querySelector(".lane-readout")?.textContent).toBe("−6.0 dB");
    expect(last()?.textContent.startsWith("Difference")).toBe(true);
    expect(last()?.querySelector(".align")?.getAttribute("aria-pressed")).toBe(
      "true",
    );
  });
});
