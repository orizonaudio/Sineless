// Compare on the real engine's frames, two unrelated noises: the engine finds no clear match
// (no offset, polarity 0), and the page says the other track is unrelated.
import { beforeAll, describe, expect, test } from "vitest";

import { laneReadout } from "./compare-readout.ts";
import { settle, startRecorded } from "./harness.ts";

describe("a recorded Compare of two unrelated noises", () => {
  beforeAll(async () => {
    await startRecorded("CompareUnrelated_48k");
  });

  test("calls the other track unrelated", async () => {
    await settle();
    await expect
      .poll(() => laneReadout("Other noise"))
      .toMatch(/^unrelated · /u);
  });
});
