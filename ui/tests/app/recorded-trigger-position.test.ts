// The engine's frames at a trigger Position of 50%: the page draws the dashed line halfway across
// and counts the time labels from it, as the frames say, with no setting of its own to go by.
import { describe, expect, test } from "vitest";

import { beforeApp, byId, settle, startRecorded } from "./harness.ts";

const minus = "−";

describe("a recorded session at a trigger position of 50%", () => {
  beforeApp(async () => {
    await startRecorded("TriggerPosition50_48k");
  });

  test("draws the trigger line halfway and counts time from it", async () => {
    await settle();
    expect(window.orizonDebug?.lastFrame?.triggerPosition).toBeCloseTo(0.5, 6);
    expect(Number.parseFloat(byId("trigger-line").style.left)).toBeCloseTo(
      50,
      3,
    );
    const labels = [...byId("labels").querySelectorAll(".axis-label")]
      .map((label) => label.textContent)
      .filter((text) => /s$/u.test(text));
    expect(labels).toEqual([`${minus}3 ms`, `${minus}1 ms`, "1 ms", "3 ms"]);
  });
});
