// What a page in Multi or Compare shows on the real engine's frames from a host that reports no
// tempo: no bar placed, a chip says why, and the readouts name this track. (The frame carries the
// group's lane set, which the mock's does not, but no lanes; the recording itself is kept to what
// the engine sends by editor/tests/SessionRecordingTests.cpp.)
import { beforeAll, expect, test } from "vitest";

import { byId, chips, settle, startRecorded } from "./harness.ts";

export function noTempoTests(scenario: string): void {
  beforeAll(async () => {
    await startRecorded(scenario);
  });

  test("says there is no tempo, and places no bars", async () => {
    await settle();
    await expect.poll(chips).toContain("No tempo");
    expect(document.querySelectorAll(".axis-detail")).toHaveLength(0);
  });

  test("names what the readouts measure: this track", async () => {
    await settle();
    expect(byId("readout-tag").textContent).toBe("This track");
  });
}
