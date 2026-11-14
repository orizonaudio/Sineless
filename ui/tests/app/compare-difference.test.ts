// Compare's last lane showing the difference of the checked tracks instead of their sum, through
// the real page: the pair on the lane's label chooses, the frame says which lane it carries, and
// the label follows the frame (its name, its figure, and Preview fix, which is the sum's).
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

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
