// The multi-track and Compare views, through the real page: lane labels from the engine's lanes
// messages, the trigger marker in this track's lane, and the Compare readouts.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, choice, settle, startApp } from "./harness.ts";

function laneNames(): string[] {
  return [...byId("lane-labels").querySelectorAll(".lane-label")].map(
    (label) => label.textContent,
  );
}

function compareCheck(name: string): HTMLButtonElement {
  const check = [...byId("lane-labels").querySelectorAll("button")].find(
    (button) =>
      button.classList.contains("compare-check") && button.textContent === name,
  );
  expect(check, `check ${name}`).toBeDefined();
  return check as HTMLButtonElement;
}

// Multi or Compare, Seconds, stacked, with R as the Single view's trigger source: R's lane is not
// the first, so a marker placed by the views rather than the tracks would show.
async function show(mode: "Multi" | "Compare"): Promise<void> {
  await userEvent.click(choice("time-base", "Sec"));
  await userEvent.click(choice("display-mode", "Single"));
  if (byId("trigger-popover").hidden) {
    await userEvent.click(byId("trigger-details"));
  }
  await userEvent.click(choice("source", "R"));
  await userEvent.click(choice("group", "A"));
  await userEvent.click(choice("display-mode", mode));
  await userEvent.click(choice("layout", "Stack"));
  await settle();
}

// Checks or unchecks a track for comparison. Each click asks the engine, whose new lanes rebuild
// the labels: one at a time.
async function setCompared(name: string, compared: boolean): Promise<void> {
  const state = () => compareCheck(name).getAttribute("aria-checked");
  if (state() !== String(compared)) {
    await userEvent.click(compareCheck(name));
  }
  await expect.poll(state).toBe(String(compared));
}

beforeAll(async () => {
  await startApp();
});

describe("the multi-track view", () => {
  test("labels each lane with its track, and stacks them", async () => {
    await show("Multi");
    expect(window.orizonDebug?.lastFrame?.lanes).toHaveLength(4);
    // The hi-hats have no name: their lane is named by its place.
    await expect.poll(laneNames).toEqual(["Kick", "Bass", "Track 3", "Kick 2"]);
  });

  test("puts the trigger marker in this track's lane", async () => {
    await show("Multi");
    // Kick, this instance's track, is the first of four stacked lanes; R would be the second.
    const screen = byId("screen").getBoundingClientRect();
    const marker = byId("trigger-marker").getBoundingClientRect();
    const y = (marker.top + marker.height / 2 - screen.top) / screen.height;
    expect(y).toBeGreaterThan(0);
    expect(y).toBeLessThan(0.25);
  });

  test("shows no frequency: a multi-track frame carries none", async () => {
    await show("Multi");
    await expect.element(byId("frequency")).toHaveTextContent("—");
  });
});

describe("the Compare view", () => {
  test("always compares this track, and adds the sum", async () => {
    await show("Compare");
    await expect.element(compareCheck("Kick")).toBeDisabled(); // This track: always compared.
    await expect
      .element(compareCheck("Kick"))
      .toHaveAttribute("aria-checked", "true");
    await expect
      .poll(() => laneNames().some((name) => name.startsWith("Sum")))
      .toBe(true);
  });

  test("finds Kick 2 late and inverted, and the fix lifts the sum", async () => {
    await show("Compare");
    await setCompared("Bass", false);
    await setCompared("Kick 2", true);
    await settle();
    const readout = () =>
      compareCheck("Kick 2").parentElement?.querySelector(".lane-readout")
        ?.textContent;
    // 62 samples at 48 kHz, as the mock delays it 1.3 ms.
    await expect.poll(readout).toBe("+1.3 ms Ø · −0.93");
    const sum = () =>
      byId("lane-labels").querySelector<HTMLElement>("[data-help='sumGain']");
    await expect.poll(() => sum()?.classList.contains("cancels")).toBe(true);

    const preview =
      byId("lane-labels").querySelector<HTMLElement>(".preview-fix");
    await userEvent.click(preview as HTMLElement);
    await settle();
    await expect.poll(() => sum()?.classList.contains("reinforces")).toBe(true);
  });

  test("says when the group has no other track", async () => {
    await show("Compare");
    await userEvent.click(choice("group", "B"));
    await settle();
    await expect
      .poll(
        () => byId("lane-labels").querySelector(".compare-note")?.textContent,
      )
      .toBe(
        "No other track in group B to compare with. Insert Sineless on another track.",
      );
  });
});
