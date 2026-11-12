// Multi's Labels switch (#407), through the real page and the mock back end: Off hides the lane
// labels, stacked and overlaid, live and frozen, with several tracks or one; Compare's labels hold
// its check boxes and readouts, so they show whatever Labels says while Compare is on, and the row
// is greyed out keeping its choice; Single has no lane labels and no Labels row in reach; the
// choice is the engine's, so the page reopened shows it. No case waits for new lanes after the
// switch it tests: a track alone in its group gets none.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  byId,
  chips,
  choice,
  press,
  setCompare,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

const layouts = ["Stack", "Overlay"] as const;
const states = [
  ["live", false],
  ["frozen", true],
] as const;
// Group A holds the mock's several tracks; group B this instance's track alone.
const groups = [
  ["several tracks", "A", ["Kick", "Bass", "Track 3", "Kick 2"]],
  ["one track", "B", ["Kick"]],
] as const;

// The lane names the labels show, in order: one per stacked lane, or the overlaid legend's.
function shownNames(): string[] {
  return [
    ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label > span"),
  ].map((name) => name.textContent);
}

function labelCount(): number {
  return byId("lane-labels").querySelectorAll(".lane-label").length;
}

function compareChecks(): number {
  return byId("lane-labels").querySelectorAll(".compare-check").length;
}

function labelsRow(label: "Off" | "On"): HTMLButtonElement {
  return choice("lane-labels-choice", label);
}

async function setLabels(on: boolean): Promise<void> {
  const button = labelsRow(on ? "On" : "Off");
  await press(button);
  await expect.element(button).toHaveAttribute("aria-pressed", "true");
}

// Multi, Trig, in a group and layout, Compare off, its labels shown; then frozen if asked.
async function multi(
  group: string,
  layout: string,
  frozen: boolean,
): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Multi"));
  await press(choice("group", group));
  await setCompare(false);
  await press(choice("layout", layout));
  await settle();
  await expect.poll(labelCount).toBeGreaterThan(0);
  if (frozen) {
    await userEvent.click(byId("freeze"));
    // The frozen frame can take longer than the matcher's default second in a busy browser.
    await expect
      .poll(() => byId("freeze").textContent, { timeout: 5000 })
      .toBe("Frozen");
  }
}

describe.each(groups)("with %s", (_, group, names) => {
  describe.each(layouts)("%s", (layout) => {
    describe.each(states)("%s", (_, frozen) => {
      test("Off hides the lane labels and On shows them again", async () => {
        await startApp();
        await multi(group, layout, frozen);
        expect(shownNames()).toEqual(names);
        await setLabels(false);
        await expect.poll(labelCount).toBe(0);
        await setLabels(true);
        await expect.poll(shownNames).toEqual(names);
      });

      test("Compare shows them whatever Labels says, and greys out the row keeping Off", async () => {
        await startApp();
        await multi(group, layout, frozen);
        await setLabels(false);
        await expect.poll(labelCount).toBe(0);

        await setCompare(true);
        await expect.poll(labelCount).toBeGreaterThan(0);
        if (names.length > 1) {
          await expect.poll(compareChecks).toBeGreaterThan(0);
        } else {
          await expect
            .poll(
              () =>
                byId("lane-labels").querySelector(".compare-note")
                  ?.textContent ?? "",
            )
            .toContain("No other track in group B");
        }
        expect(labelsRow("Off").disabled).toBe(true);
        expect(labelsRow("On").disabled).toBe(true);
        expect(labelsRow("Off").getAttribute("aria-pressed")).toBe("true");

        await setCompare(false);
        await expect.poll(labelCount).toBe(0);
        expect(labelsRow("Off").disabled).toBe(false);
        expect(labelsRow("On").disabled).toBe(false);
      });
    });
  });
});

describe("the Labels row", () => {
  test("is in Multi's Views popover, and asks the engine for the choice", async () => {
    const app = await startApp();
    await press(choice("display-mode", "Multi"));
    expect(byId("group-popover").contains(labelsRow("Off"))).toBe(true);
    await press(labelsRow("Off"));
    const sent = app.sent.filter((message) => message.type === "setSetting");
    expect(sent.at(-1)).toEqual({
      type: "setSetting",
      name: "laneLabels",
      value: "off",
    });
  });

  test("is out of reach in Single, which shows no lane labels, and applies back in Multi", async () => {
    const app = await startApp();
    await multi("A", "Stack", false);
    await press(choice("display-mode", "Single"));
    await settle();
    expect(byId("group-group").inert).toBe(true);
    expect(labelCount()).toBe(0);
    // Chosen while Single shows (a host's automation or undo), it holds once Multi is back.
    app.transport.send({
      type: "setSetting",
      name: "laneLabels",
      value: "off",
    });
    await settle();
    await press(choice("display-mode", "Multi"));
    await settle();
    expect(labelCount()).toBe(0);
    app.transport.send({ type: "setSetting", name: "laneLabels", value: "on" });
    await expect.poll(labelCount).toBeGreaterThan(0);
  });

  test("leaves the Views field's summary as it is", async () => {
    await startApp();
    await press(choice("display-mode", "Multi"));
    const summary = () => byId("group-summary").textContent;
    const label = () => byId("group-details").getAttribute("aria-label");
    const [on, onLabel] = [summary(), label()];
    await setLabels(false);
    expect(summary()).toBe(on);
    expect(label()).toBe(onLabel);
  });

  test("is the engine's: the page reopened shows the labels off", async () => {
    const first: Started = await startApp();
    await multi("A", "Stack", false);
    await setLabels(false);
    await expect.poll(labelCount).toBe(0);
    await startApp({}, first.transport);
    await press(choice("display-mode", "Multi"));
    await settle();
    expect(labelsRow("Off").getAttribute("aria-pressed")).toBe("true");
    expect(labelCount()).toBe(0);
  });
});

// With Labels Off no lane is named, so the status chip alone reports samples that are not finite;
// On, the lane holding them is named in the warning colour.
describe("a track sending samples that are not finite", () => {
  test("is reported by the chip with Labels Off, and its lane named with Labels On", async () => {
    await startApp({ brokenSamples: 3 });
    await multi("A", "Stack", false);
    const marked = () =>
      [
        ...byId("lane-labels").querySelectorAll(".lane-label > span.invalid"),
      ].map((name) => name.textContent);
    await expect.poll(marked).toEqual(["Kick"]);
    await setLabels(false);
    await expect.poll(labelCount).toBe(0);
    await settle();
    expect(chips()).toContain("Invalid samples");
    await setLabels(true);
    await expect.poll(marked).toEqual(["Kick"]);
  });
});
