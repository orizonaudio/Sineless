// The multi-track view on the real engine's frames: lanes for the group's three tracks, named as
// the host reports them in the lanes message the session sends after the first frame.
import { beforeAll, describe, expect, test } from "vitest";

import { byId, settle, startRecorded } from "./harness.ts";

describe("a recorded group of three named tracks", () => {
  beforeAll(async () => {
    await startRecorded("MultiThreeTracks_48k");
  });

  test("names a lane for each track", async () => {
    await settle();
    await expect
      .poll(() =>
        [...byId("lane-labels").querySelectorAll(".lane-label")].map(
          (label) => label.textContent,
        ),
      )
      .toEqual(["Kick", "Bass", "Keys"]);
    expect(window.orizonDebug?.lastFrame?.lanes).toHaveLength(3);
  });
});
