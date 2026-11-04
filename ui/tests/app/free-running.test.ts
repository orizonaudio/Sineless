// The time base chooses how the window is placed: Beats sweeps, Free runs free, Trig is placed by
// the trigger. Only the controls that apply are shown: the trigger's in Trig only, each time base
// with its own span. A free-running frame counts back from its right edge, live and frozen, hides
// the trigger marker and line, and names no trigger mode; the page follows the frame's flag, as
// the engine decides, not the settings.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { frozenHoldSeconds } from "../../../protocol/src/limits.ts";
import { valueNames } from "../../../protocol/src/setting-values.ts";
import {
  byId,
  choice,
  chips,
  inTimeBase,
  press,
  settle,
  spanFieldOf,
  startApp,
  typeInto,
} from "./harness.ts";

// The time-base control's label for each time base of the protocol.
const labelOf: Readonly<Record<string, string>> = {
  beats: "Beats",
  free: "Free",
  trigger: "Trig",
};

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

// The text a span pill shows.
function spanShown(host: string): string {
  return byId(host).querySelector(".field")?.textContent ?? "";
}

describe("the time base", () => {
  beforeAll(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
  });

  test.each(valueNames("TimeBase"))(
    "%s shows its own span and the trigger only where it applies",
    async (name) => {
      const label = labelOf[name] ?? name;
      await inTimeBase(label);
      for (const [other, host] of Object.entries(spanFieldOf)) {
        expect(byId(host).hidden, host).toBe(other !== label);
      }
      // The time pill's help names the span shown.
      expect(byId("time-group").dataset.help).toBe(
        { beats: "beatSpan", free: "freeSpan", trigger: "triggerSpan" }[name],
      );
      const triggered = name === "trigger";
      expect(byId("trigger-group").hidden).toBe(!triggered);
      expect(byId("trigger-divider").hidden).toBe(!triggered);
    },
  );

  // Re-arm belongs to Single, which only Trig applies.
  test("shows Re-arm in Trig with Single only, not in Free with Single left on", async () => {
    await inTimeBase("Trig");
    await press(choice("mode", "Single"));
    await expect.element(byId("arm")).toBeVisible();
    await inTimeBase("Free");
    await expect.element(byId("arm")).not.toBeVisible();
    await inTimeBase("Trig");
    await expect.element(byId("arm")).toBeVisible();
    await press(choice("mode", "Auto"));
    await expect.element(byId("arm")).not.toBeVisible();
  });

  test.each(["Free", "Beats"])(
    "closes a trigger popover left open on going to %s",
    async (label) => {
      await inTimeBase("Trig");
      await userEvent.click(byId("trigger-details"));
      expect(byId("trigger-popover").hidden).toBe(false);
      await inTimeBase(label);
      expect(byId("trigger-popover").hidden).toBe(true);
    },
  );

  test("brings back each time base's own span", async () => {
    await inTimeBase("Trig");
    await typeInto("span-field", "200 ms");
    await inTimeBase("Free");
    await typeInto("free-span-field", "2 s");
    await inTimeBase("Trig");
    await expect.poll(() => spanShown("span-field")).toBe("200 ms");
    await inTimeBase("Free");
    await expect.poll(() => spanShown("free-span-field")).toBe("2 s");
  });
});

describe("a free-running window", () => {
  beforeAll(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Trig"));
    await press(choice("mode", "Normal"));
  });

  test("in Free, counts back from the right edge, hides the trigger and names no mode", async () => {
    await inTimeBase("Free");
    await typeInto("free-span-field", "500 ms");
    await settle();
    await expect.poll(countsBack).toBe(true);
    expect(byId("trigger-marker").hidden).toBe(true);
    expect(byId("trigger-line").hidden).toBe(true);
    await expect.poll(chips).toEqual([]);
  });

  test("frozen in Free, still counts back", async () => {
    await press(byId("freeze"));
    await settle();
    await expect.poll(chips).toContain("Frozen");
    await expect.poll(countsBack).toBe(true);
    await press(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
  });

  // Trig chosen while frozen in Free shows its span ending where the free window ended, so it
  // reaches back as far as the audio held allows: the hold, less the span.
  test("frozen in Free at 4 s, then Trig at 1 s, ends where Free ended", async () => {
    await typeInto("free-span-field", "4 s");
    await press(byId("freeze"));
    await expect.poll(chips).toContain("Frozen");
    await inTimeBase("Trig");
    await typeInto("span-field", "1 s");
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.freeRunning)
      .toBe(false);
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.panReach ?? 0)
      .toBeCloseTo(frozenHoldSeconds - 1, 2);
    await press(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
    await inTimeBase("Free");
  });

  test("in Trig again, counts from the trigger point and shows its line", async () => {
    await inTimeBase("Trig");
    await press(choice("mode", "Auto"));
    await typeInto("span-field", "500 ms");
    await settle();
    await expect.poll(countsBack).toBe(false);
    await expect.poll(chips).toContain("Auto");
    await expect.poll(() => byId("trigger-line").hidden).toBe(false);
  });
});
