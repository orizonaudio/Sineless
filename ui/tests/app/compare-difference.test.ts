// Compare's last lane showing the difference of the checked tracks instead of their sum, through
// the real page: the pair on the lane's label chooses, the frame says which lane it carries, and
// the label follows the frame (its name, its figure, and Preview fix, which is the sum's, or
// Align, which is the difference's).
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { laneReadout } from "./compare-readout.ts";
import {
  beforeApp,
  byId,
  choice,
  multiSummary,
  press,
  setCompare,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

let started: Started;

beforeApp(async () => {
  started = await startApp();
});

function lastLabel(): HTMLElement {
  const labels =
    byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label > span");
  return labels[labels.length - 1] as HTMLElement;
}

function pick(name: "Sum" | "Difference"): HTMLButtonElement {
  const button = lastLabel().querySelector<HTMLButtonElement>(
    `.compare-lane button[aria-label="${name}"]`,
  );
  expect(button, `the ${name} choice`).not.toBeNull();
  return button as HTMLButtonElement;
}

const figure = () => lastLabel().querySelector<HTMLElement>(".lane-readout");
const previewFix = () => lastLabel().querySelector<HTMLElement>(".preview-fix");
const align = () => lastLabel().querySelector<HTMLElement>(".align");
const readoutHelp = (name: string) =>
  [...byId("lane-labels").querySelectorAll(".compare-check")]
    .find((button) => button.textContent === name)
    ?.parentElement?.querySelector<HTMLElement>(".lane-readout")?.dataset[
    "help"
  ];
// Checks or unchecks a track for comparison: the engine's new lanes rebuild the labels.
async function setCompared(name: string, compared: boolean): Promise<void> {
  const check = () =>
    [
      ...byId("lane-labels").querySelectorAll<HTMLButtonElement>(
        ".compare-check",
      ),
    ].find((button) => button.textContent === name);
  if (check()?.getAttribute("aria-checked") !== String(compared)) {
    await userEvent.click(check() as HTMLButtonElement);
  }
  await expect
    .poll(() => check()?.getAttribute("aria-checked"))
    .toBe(String(compared));
}
// The last lane's columns as the page drew them last: how tall the tallest is.
const tallest = () => {
  const frame = window.orizonDebug?.lastFrame;
  const lane = frame?.lanes.at(-1);
  return lane === undefined
    ? Number.NaN
    : lane.reduce((most, value) => Math.max(most, Math.abs(value)), 0);
};

async function showCompare(): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Multi"));
  await press(choice("group", "A"));
  await press(choice("layout", "Stack"));
  await setCompare(true);
  await settle();
}

async function show(name: "Sum" | "Difference"): Promise<void> {
  if (pick(name).getAttribute("aria-pressed") !== "true") {
    await userEvent.click(pick(name));
  }
  await expect.poll(() => pick(name).getAttribute("aria-pressed")).toBe("true");
}

describe("Compare's last lane", () => {
  test("shows the sum until asked, with its gain and Preview fix", async () => {
    await showCompare();
    await show("Sum");
    expect(pick("Difference").getAttribute("aria-pressed")).toBe("false");
    expect(lastLabel().textContent.startsWith("Sum")).toBe(true);
    expect(figure()?.dataset["help"]).toBe("sumGain");
    expect(previewFix()).not.toBeNull();
  });

  test("shows the difference when asked: its name, how deep it nulls, and no Preview fix", async () => {
    await showCompare();
    await show("Difference");
    expect(
      started.sent.some(
        (message) =>
          message.type === "setSetting" &&
          message["name"] === "compareLane" &&
          message["value"] === "difference",
      ),
    ).toBe(true);
    expect(window.orizonDebug?.lastFrame?.comparison?.lane).toBe(1);
    expect(lastLabel().textContent.startsWith("Difference")).toBe(true);
    expect(figure()?.dataset["help"]).toBe("nullDepth");
    // A level against this track, in dB, with neither the sum's yellow nor its emphasis.
    await expect.poll(() => figure()?.textContent).toMatch(/dB$/u);
    expect(figure()?.className).toBe("lane-readout");
    expect(previewFix()).toBeNull();
    await show("Sum");
  });

  test("resets Preview fix when the difference takes the sum's place", async () => {
    await showCompare();
    await show("Sum");
    await userEvent.click(previewFix() as HTMLElement);
    await expect
      .poll(() => previewFix()?.getAttribute("aria-pressed"))
      .toBe("true");
    await show("Difference");
    const fixes = started.sent.filter(
      (message) => message.type === "previewFix",
    );
    expect(fixes.at(-1)?.["on"]).toBe(false);
    await show("Sum");
    expect(previewFix()?.getAttribute("aria-pressed")).toBe("false");
  });

  // The second kick microphone is the kick flipped and 1.3 ms late: beside the difference its
  // readout says what lining it up takes out, where the sum's says how a fix would shift it and
  // how alike the two are.
  test("says what Align takes out of each track: its delay and a flip", async () => {
    await showCompare();
    await show("Sum");
    await setCompared("Kick 2", true);
    await expect.poll(() => laneReadout("Kick 2")).toBe("+1.3 ms Ø · −0.93");
    expect(readoutHelp("Kick 2")).toBe("match");
    await show("Difference");
    await expect.poll(() => laneReadout("Kick 2")).toBe("+1.3 ms · Ø");
    expect(readoutHelp("Kick 2")).toBe("matchDifference");
    // A track that matches this one nowhere has nothing to take out.
    expect(laneReadout("Bass")).toBe("unrelated");
    await show("Sum");
    await setCompared("Kick 2", false);
  });

  // Align is the difference's switch, on until switched off, where the sum has Preview fix. It
  // sets the setting, and the difference follows: lined up, the second kick microphone, at 0.8 of
  // the kick's level, leaves a fifth of the kick; as recorded, flipped and late, it adds to it.
  test("has Align on the difference's label, which sets the setting", async () => {
    await showCompare();
    await show("Sum");
    expect(align()).toBeNull();
    await setCompared("Kick 2", true);
    await setCompared("Bass", false);
    await show("Difference");
    expect(align()?.dataset["help"]).toBe("align");
    expect(align()?.getAttribute("aria-pressed")).toBe("true");
    await settle();
    const linedUp = tallest();
    await userEvent.click(align() as HTMLElement);
    await expect
      .poll(() => align()?.getAttribute("aria-pressed"))
      .toBe("false");
    expect(
      started.sent.some(
        (message) =>
          message.type === "setSetting" &&
          message["name"] === "align" &&
          message["value"] === "off",
      ),
    ).toBe(true);
    await settle();
    expect(tallest()).toBeGreaterThan(linedUp + 0.2);
    await userEvent.click(align() as HTMLElement);
    await expect.poll(() => align()?.getAttribute("aria-pressed")).toBe("true");
    await show("Sum");
    await setCompared("Bass", true);
    await setCompared("Kick 2", false);
  });

  test("leaves the Views field as it is: Σ there stands for Compare", async () => {
    await showCompare();
    await show("Sum");
    const summary = multiSummary();
    const width = byId("views-summary").getBoundingClientRect().width;
    await show("Difference");
    expect(multiSummary()).toBe(summary);
    expect(byId("views-summary").getBoundingClientRect().width).toBe(width);
    await show("Sum");
  });
});
