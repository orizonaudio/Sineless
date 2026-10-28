// A help tooltip waits for the pointer to rest. A drag is no rest: a tooltip shown mid-drag would
// cover the value being dragged, or float under nothing while a value drag hides the pointer.
import { beforeAll, describe, expect, test, vi } from "vitest";

import { byId, startApp } from "./harness.ts";

// The help switch explains itself even while help is off.
function moveOverHelpSwitch(buttons: number): void {
  const target = byId("help");
  const box = target.getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons,
      clientX: box.left + box.width / 2,
      clientY: box.top + box.height / 2,
    }),
  );
}

describe("help tooltips", () => {
  beforeAll(async () => {
    await startApp();
  });

  test("appear once the pointer rests with no button held", async () => {
    const tip = byId("help-tip");
    moveOverHelpSwitch(0);
    // Timers run late in a busy browser: the wait outlasts the delay several times over.
    await vi.waitFor(
      () => {
        expect(tip.hidden).toBe(false);
      },
      { timeout: 5000 },
    );
  });

  test("do not appear while a button is held, as during a drag", async () => {
    const tip = byId("help-tip");
    moveOverHelpSwitch(1);
    expect(tip.hidden).toBe(true);
    // Longer than the tooltip's delay: one that would show has shown by then.
    await new Promise((resolve) => setTimeout(resolve, 1500));
    expect(tip.hidden).toBe(true);
  });
});
