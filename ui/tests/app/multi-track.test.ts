// The multi-track view with Compare off and on, through the real page: lane labels from the
// engine's lanes messages, the trigger marker in this track's lane, the Compare switch and its
// readouts.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  beforeApp,
  byId,
  choice,
  press,
  setCompare,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

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

// Multi with Compare off or on, Seconds, stacked, with R as the Single view's trigger source: R's
// lane is not the first, so a marker placed by the views rather than the tracks would show.
async function show(mode: "Multi" | "Compare"): Promise<void> {
  await userEvent.click(choice("time-base", "Trig"));
  await userEvent.click(choice("display-mode", "Single"));
  await press(choice("source", "R"));
  await userEvent.click(choice("display-mode", "Multi"));
  await press(choice("group", "A"));
  await setCompare(mode === "Compare");
  await userEvent.click(choice("layout", "Stack"));
  await settle();
}

function compareChecks(): number {
  return byId("lane-labels").querySelectorAll(".compare-check").length;
}

function hasSumLane(): boolean {
  return laneNames().some((name) => name.startsWith("Sum"));
}

function previewFix(): HTMLElement | null {
  return byId("lane-labels").querySelector<HTMLElement>(".preview-fix");
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

let started: Started;

beforeApp(async () => {
  started = await startApp();
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

  // The checks are the instance's: the engine keeps them, and the page reopened shows them
  // rather than going back to comparing the first other track.
  test("keeps the checked tracks when the editor reopens", async () => {
    await show("Compare");
    await setCompared("Bass", false);
    await setCompared("Kick 2", true);
    await setCompared("Track 3", true);
    started = await startApp({}, started.transport);
    const checks = () =>
      ["Bass", "Track 3", "Kick 2"].map((name) =>
        compareCheck(name).getAttribute("aria-checked"),
      );
    await expect.poll(checks).toEqual(["false", "true", "true"]);
    expect(started.sent.some((message) => message.type === "compare")).toBe(
      false,
    );
  });

  test("says when the group has no other track", async () => {
    await show("Compare");
    await press(choice("group", "B"));
    await settle();
    await expect
      .poll(
        () => byId("lane-labels").querySelector(".compare-note")?.textContent,
      )
      .toBe(
        "No other track in group B to compare with. Insert Sineless on another track.", // The branded name (vitest.config.ts).
      );
  });
});

describe("the Compare switch", () => {
  // Off, Multi shows the tracks alone: no check boxes, no sum, no note, and nothing compared.
  test("adds nothing to Multi while it is off", async () => {
    await show("Multi");
    expect(compareChecks()).toBe(0);
    expect(hasSumLane()).toBe(false);
    await press(choice("group", "B"));
    await settle();
    expect(byId("lane-labels").querySelector(".compare-note")).toBeNull();
    expect(window.orizonDebug?.lastFrame?.comparison).toBeUndefined();
    await press(choice("group", "A"));
  });

  // Left on in Single, it shows nothing there; back in Multi, Compare shows again, as the Group
  // field says.
  test("keeps its state in Single and shows Compare back in Multi", async () => {
    await show("Compare");
    await expect.poll(compareChecks).toBe(4);
    await userEvent.click(choice("display-mode", "Single"));
    await settle();
    expect(compareChecks()).toBe(0);
    expect(window.orizonDebug?.lastFrame?.comparison).toBeUndefined();
    await userEvent.click(choice("display-mode", "Multi"));
    await expect.poll(compareChecks).toBe(4);
    await expect.poll(hasSumLane).toBe(true);
    expect(byId("group-summary").textContent).toBe("A·Σ");
  });

  // Preview fix is Compare's: turning Compare off, or leaving Multi, resets it, and Compare comes
  // back without it.
  test.each(["switch", "display mode"])(
    "resets Preview fix when Compare leaves the screen by the %s",
    async (how) => {
      await show("Compare");
      await expect.poll(previewFix).not.toBeNull();
      await userEvent.click(previewFix() as HTMLElement);
      await expect
        .element(previewFix() as HTMLElement)
        .toHaveAttribute("aria-pressed", "true");
      const sent = started.sent.length;
      if (how === "switch") {
        await setCompare(false);
      } else {
        await userEvent.click(choice("display-mode", "Single"));
      }
      await expect
        .poll(() =>
          started.sent
            .slice(sent)
            .some((message) => message.type === "previewFix" && !message.on),
        )
        .toBe(true);
      if (how === "switch") {
        await setCompare(true);
      } else {
        await userEvent.click(choice("display-mode", "Multi"));
      }
      await expect.poll(previewFix).not.toBeNull();
      expect(previewFix()?.getAttribute("aria-pressed")).toBe("false");
    },
  );
});
