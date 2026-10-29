// Free and Seconds spans above the threshold are one free-running window: the time axis counts
// back from the right edge, live and frozen, the trigger marker and line are hidden, and the chip
// says "Free" whatever the trigger mode set. The page follows the frame's flag, as the engine
// decides, not the settings.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { freeRunningThresholdSeconds } from "../../../protocol/src/limits.ts";
import {
  byId,
  choice,
  chips,
  press,
  settle,
  startApp,
  typeInto,
} from "./harness.ts";

// The time labels' texts, in screen order.
function timeLabels(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.firstChild?.textContent ?? "")
    .filter((text) => text.endsWith("s"));
}

// Every time label counts back (U+2212), as free-running frames do.
function countsBack(): boolean {
  const labels = timeLabels();
  return labels.length > 0 && labels.every((text) => text.startsWith("−"));
}

describe("a free-running window", () => {
  beforeAll(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
  });

  test("in Free at a short span, counts back from the right edge and hides the trigger", async () => {
    await typeInto("span-field", "500 ms");
    await press(choice("mode", "Free"));
    await settle();
    await expect.poll(countsBack).toBe(true);
    expect(byId("trigger-marker").hidden).toBe(true);
    expect(byId("trigger-line").hidden).toBe(true);
    await expect.poll(chips).toContain("Free");
  });

  test("frozen in Free, still counts back", async () => {
    await press(byId("freeze"));
    await settle();
    await expect.poll(chips).toContain("Frozen");
    await expect.poll(countsBack).toBe(true);
    await press(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
  });

  test.each(["Auto", "Normal", "Single"])(
    "above the threshold in %s, is named Free and counts back",
    async (mode) => {
      await typeInto("span-field", "2 s");
      await press(choice("mode", mode));
      await settle();
      expect(2).toBeGreaterThan(freeRunningThresholdSeconds);
      await expect.poll(chips).toContain("Free");
      expect(chips()).not.toContain(mode);
      await expect.poll(countsBack).toBe(true);
      expect(byId("trigger-marker").hidden).toBe(true);
    },
  );

  test("triggered again at a short span, counts from the trigger point and shows its line", async () => {
    await typeInto("span-field", "500 ms");
    await press(choice("mode", "Auto"));
    await settle();
    await expect.poll(countsBack).toBe(false);
    await expect.poll(chips).toContain("Auto");
    await expect.poll(() => byId("trigger-line").hidden).toBe(false);
  });
});
