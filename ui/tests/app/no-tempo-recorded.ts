// What a page in Multi or Compare shows on the real engine's frames from a host that reports no
// tempo: the frame carries the group's lane set (the mock's carries none) but no lanes, so no lane
// is drawn and no bar placed, a chip says why, and the readouts name this track.
import { expect, test } from "vitest";

import { byId, chips, settle, startRecorded } from "./harness.ts";

export function noTempoTests(scenario: string): void {
  test("says there is no tempo, and places no bars", async () => {
    await startRecorded(scenario);
    await settle();
    await expect.poll(chips).toContain("No tempo");
    expect(document.querySelectorAll(".axis-detail")).toHaveLength(0);
  });

  test("draws no lane, and names what the readouts measure", async () => {
    await settle();
    expect(window.orizonDebug?.lastFrame?.lanes).toHaveLength(0);
    expect(window.orizonDebug?.lastFrame?.laneSet).toBeGreaterThan(0);
    expect(byId("readout-tag").textContent).toBe("This track");
  });
}
