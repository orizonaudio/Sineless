// Compare's last lane showing the difference of the checked tracks instead of their sum, through
// the real page: Compare's switch in the Views popover chooses (Off, Sum or Diff), the frame says
// which lane it carries, and the label follows the frame (its name, its figure, and Preview fix,
// which is the sum's, or Align, which is the difference's).
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

// Compare's switch, by the lane's name on the label.
function pick(name: "Sum" | "Difference"): HTMLElement {
  return choice("compare", name === "Sum" ? "Sum" : "Diff");
}

// Whether the page last asked the engine to preview the fix, or to stop.
const lastFix = () =>
  started.sent.filter((message) => message.type === "previewFix").at(-1)?.[
    "on"
  ];
// The Views field's name, as a screen reader says it.
const viewsName = () => byId("views-details").getAttribute("aria-label");
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
  await setCompare(name === "Sum" ? "Sum" : "Diff");
  await expect.poll(() => lastLabel().textContent.startsWith(name)).toBe(true);
}

describe("Compare's last lane", () => {
  test("shows the sum until asked, with its gain and Preview fix", async () => {
    await showCompare();
    await show("Sum");
    expect(pick("Difference").getAttribute("aria-pressed")).toBe("false");
    expect(lastLabel().textContent.startsWith("Sum")).toBe(true);
    expect(figure()?.dataset["help"]).toBe("sumGain");
    expect(previewFix()?.dataset["help"]).toBe("previewFix");
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
    // The engine draws the sum with the fix only once asked.
    expect(lastFix()).toBe(true);
    await show("Difference");
    expect(lastFix()).toBe(false);
    await show("Sum");
    expect(previewFix()?.getAttribute("aria-pressed")).toBe("false");
  });

  // A project the host restores on the difference changes the lane alone: no change of Compare
  // follows it, as one does when the page's own switch is pressed.
  test("follows the host when it changes the lane alone: the switch, the sign, the field's name and Preview fix", async () => {
    await showCompare();
    await show("Sum");
    await userEvent.click(previewFix() as HTMLElement);
    await expect
      .poll(() => previewFix()?.getAttribute("aria-pressed"))
      .toBe("true");
    started.transport.send({
      type: "setSetting",
      name: "compareLane",
      value: "difference",
    });
    await expect
      .element(pick("Difference"))
      .toHaveAttribute("aria-pressed", "true");
    expect(pick("Sum").getAttribute("aria-pressed")).toBe("false");
    expect(multiSummary()).toBe("A·M·Δ·");
    expect(viewsName()).toBe(
      "Views: group A, Mid, Compare difference, labels on",
    );
    expect(lastFix()).toBe(false);
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

  // The Views field says which lane Compare adds, Σ or Δ, and is as wide with either, so nothing
  // after it in the toolbar moves; with Compare off the sign of the lane last chosen stays,
  // dimmed, and Compare comes back on that lane.
  test("names the lane in the Views field, at one width, and keeps it while off", async () => {
    await showCompare();
    await show("Sum");
    expect(multiSummary()).toBe("A·M·Σ·");
    expect(viewsName()).toBe("Views: group A, Mid, Compare sum, labels on");
    const width = byId("views-summary").getBoundingClientRect().width;
    await show("Difference");
    expect(multiSummary()).toBe("A·M·Δ·");
    expect(viewsName()).toBe(
      "Views: group A, Mid, Compare difference, labels on",
    );
    expect(byId("views-summary").getBoundingClientRect().width).toBe(width);
    await setCompare(false);
    expect(multiSummary()).toBe("A·M·Δ·");
    expect(pick("Difference").getAttribute("aria-pressed")).toBe("false");
    await show("Sum");
  });

  // The lane's label holds no choice of its own any more: its name, its figure and one switch.
  test("has no sum or difference choice on the lane's label", async () => {
    await showCompare();
    await show("Difference");
    const buttons = [...lastLabel().querySelectorAll("button")].map(
      (button) => button.textContent,
    );
    expect(buttons).toEqual(["Align"]);
    await show("Sum");
    expect(
      [...lastLabel().querySelectorAll("button")].map(
        (button) => button.textContent,
      ),
    ).toEqual(["Preview fix"]);
  });
});
