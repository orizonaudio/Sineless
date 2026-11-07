// A help tooltip waits for the pointer to rest. A drag is no rest: a tooltip shown mid-drag would
// cover the value being dragged, or float under nothing while a value drag hides the pointer.
import { afterAll, describe, expect, test, vi } from "vitest";
import { page, userEvent } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { helpTexts } from "../../src/view/help.ts";
import { beforeApp, byId, startApp } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

// Moves the pointer over an element, `dx` px right of its centre.
function moveOver(id: string, buttons = 0, dx = 0): void {
  const target = byId(id);
  const box = target.getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons,
      clientX: box.left + box.width / 2 + dx,
      clientY: box.top + box.height / 2,
    }),
  );
}

// The help switch explains itself even while help is off.
function moveOverHelpSwitch(buttons: number): void {
  moveOver("help", buttons);
}

// Longer than the tooltip's delay: one that would show has shown by then.
const pastDelay = () => new Promise((resolve) => setTimeout(resolve, 1500));

const tip = () => byId("help-tip");
const explainsHelpOn = () =>
  !tip().hidden && tip().textContent.includes(helpTexts.helpOn.text);

describe("help tooltips", () => {
  beforeApp(async () => {
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
    await pastDelay();
    expect(tip.hidden).toBe(true);
  });
});

// FR-13: a click on ? that turns help on says so at once, by the switch, so a user who clicks it
// learns what it does without waiting on it; it goes the moment the user moves on to a control.
describe("turning help on", () => {
  // Help is off whenever the editor opens: each test starts one.
  async function turnOn(): Promise<void> {
    await startApp();
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
  }

  test.each([
    { width: minimumEditorWidth, height: 420 },
    { width: defaultEditorWidth, height: 580 },
  ])(
    "explains it at once, above the switch and inside the window, at $width × $height",
    async ({ width, height }) => {
      await page.viewport(width, height);
      await turnOn();
      expect(explainsHelpOn()).toBe(true);
      const box = tip().getBoundingClientRect();
      const button = byId("help").getBoundingClientRect();
      expect(box.bottom).toBeLessThanOrEqual(button.top);
      // Just above it, right edges aligned.
      expect(button.top - box.bottom).toBeLessThan(button.height);
      expect(box.right).toBeCloseTo(button.right, -0.5);
      expect(box.left).toBeGreaterThanOrEqual(0);
      expect(box.right).toBeLessThanOrEqual(width);
      expect(box.top).toBeGreaterThanOrEqual(0);
    },
  );

  // A hand moves a little after a click: the explanation neither goes nor gives way to the
  // switch's own tip, which would drop "Click ? again".
  test("keeps the explanation through a small move over the switch", async () => {
    await turnOn();
    moveOver("help", 0, 3);
    await pastDelay();
    expect(explainsHelpOn()).toBe(true);
  });

  test("closes it when ? is clicked again, which turns help off", async () => {
    await turnOn();
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "false");
    expect(tip().hidden).toBe(true);
  });

  test("closes it on a click elsewhere", async () => {
    await turnOn();
    await userEvent.click(byId("screen"));
    expect(tip().hidden).toBe(true);
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
  });

  // The expected path: the user moves to a control, rests, and sees help working.
  test("closes it when the pointer leaves the switch, and the next rest explains a control", async () => {
    await turnOn();
    byId("help").dispatchEvent(new PointerEvent("pointerleave"));
    expect(tip().hidden).toBe(true);
    moveOver("freeze");
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts.freeze.title);
      },
      { timeout: 5000 },
    );
  });

  // Turned on from the keyboard, the pointer is elsewhere: its first move takes the
  // explanation away and shows the control under it.
  test("from the keyboard, closes it at the first move over another control", async () => {
    await startApp();
    byId("help").focus();
    await userEvent.keyboard("{Enter}");
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
    expect(explainsHelpOn()).toBe(true);
    moveOver("freeze");
    expect(explainsHelpOn()).toBe(false);
    await vi.waitFor(
      () => {
        expect(tip().textContent).toContain(helpTexts.freeze.title);
      },
      { timeout: 5000 },
    );
  });

  test("shows nothing when help is turned off", async () => {
    await turnOn();
    await userEvent.click(byId("help"));
    expect(tip().hidden).toBe(true);
    await pastDelay();
    expect(explainsHelpOn()).toBe(false);
  });
});
