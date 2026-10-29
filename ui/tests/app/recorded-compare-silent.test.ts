// Compare on the real engine's frames, the other track silent: the two never sound together, so
// the engine measures nothing (NaN), and the page shows no value rather than a 0.
import { beforeAll, describe, expect, test } from "vitest";

import { laneReadout } from "./compare-readout.ts";
import { settle, startRecorded } from "./harness.ts";

describe("a recorded Compare against a silent track", () => {
  beforeAll(async () => {
    await startRecorded("CompareOtherSilent_48k");
  });

  test("shows no value for the silent track's match", async () => {
    await settle();
    await expect.poll(() => laneReadout("Silent")).toBe("—");
  });
});
