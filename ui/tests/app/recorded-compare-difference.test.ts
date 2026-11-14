// Compare showing the difference, on the real engine's frames: the other track is this one at
// half its level, so the engine's last lane is this track at half and it nulls 6 dB below this
// track; the page names the lane and shows the engine's figure.
import { describe, expect, test } from "vitest";

import { beforeApp, byId, settle, startRecorded } from "./harness.ts";

describe("a recorded Compare showing the difference", () => {
  beforeApp(async () => {
    await startRecorded("CompareDifference_48k");
  });

  test("names the lane and shows how deep it nulls", async () => {
    await settle();
    const last = () => {
      const labels = byId("lane-labels").querySelectorAll(".lane-label > span");
      return labels[labels.length - 1];
    };
    await expect
      .poll(() => last()?.querySelector(".lane-readout")?.textContent)
      .toBe("−6.0 dB");
    expect(last()?.textContent.startsWith("Difference")).toBe(true);
    // Preview fix is the sum's.
    expect(last()?.querySelector(".preview-fix")).toBeNull();
  });
});
