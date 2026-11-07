// The trigger Position: the dashed line and the time labels sit where the frame says the trigger
// point is, live and frozen, including where a frozen view could not put it where the setting asks.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { fullView } from "../../src/view/zoom.ts";
import { timeLabels } from "../../src/view/axis-labels.ts";
import {
  beforeApp,
  byId,
  chips,
  inTimeBase,
  settle,
  startApp,
  typeInto,
} from "./harness.ts";

const minus = "−";

const lastFrame = () => window.orizonDebug?.lastFrame;

// The trigger line's place across the screen, 0 to 1.
const lineAt = () => Number.parseFloat(byId("trigger-line").style.left) / 100;

const labelTexts = () =>
  [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.textContent)
    .filter((text) => /s$/u.test(text));

// Types `text` into a field of the Trigger popover, opened.
async function typeInTrigger(host: string, text: string): Promise<void> {
  await userEvent.click(byId("wordmark"));
  const popover = byId(host).closest<HTMLElement>(".popover");
  if (popover?.hidden === true) {
    await userEvent.click(byId(popover.id.replace(/-popover$/u, "-details")));
  }
  await typeInto(host, text);
  await userEvent.click(byId("wordmark"));
}

describe("the trigger position", () => {
  beforeApp(async () => {
    await startApp();
    await inTimeBase("Trig");
    await typeInto("span-field", "100 ms");
    await settle();
  });

  test("moves the trigger line and the time labels' zero", async () => {
    await typeInTrigger("position-field", "50%");
    await expect.poll(() => lastFrame()?.triggerPosition).toBeCloseTo(0.5, 6);
    await settle();
    expect(lineAt()).toBeCloseTo(0.5, 3);
    expect(byId("trigger-line").hidden).toBe(false);
    expect(labelTexts()).toEqual([
      `${minus}30 ms`,
      `${minus}10 ms`,
      "10 ms",
      "30 ms",
    ]);
  });

  test("draws a frozen trigger point where the frame has it, not where the setting asks", async () => {
    await typeInTrigger("position-field", "10%");
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).toContain("Frozen");
    // At 0% and 500 ms the trigger would need 500 ms after it; about 90 ms is held.
    await typeInTrigger("position-field", "0%");
    await typeInto("span-field", "500 ms");
    await expect
      .poll(() => lastFrame()?.triggerPosition ?? 0)
      .toBeGreaterThan(0.5);
    await settle();
    const place = lastFrame()?.triggerPosition ?? Number.NaN;
    expect(lineAt()).toBeCloseTo(place, 3);
    expect(labelTexts()).toEqual(
      timeLabels(0.5, false, fullView, place).map((label) => label.text),
    );
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
  });
});
