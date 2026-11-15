// The Labels switch (#407, #425), through the real page and the mock back end: Off hides the lane
// labels, stacked and overlaid, live and frozen, with several tracks or one, and in Single, whose
// labels name its views; with Compare on it hides the tracks' labels and their match readouts
// too, and Compare's own lane keeps its label, which holds its figure and its switch; the choice
// is the engine's, so the page reopened shows it. No case waits for new lanes after the switch it
// tests: a track shown alone gets none.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  byId,
  chips,
  choice,
  freezeLabel,
  press,
  setCompare,
  settle,
  showOnly,
  showViews,
  startApp,
  type Started,
} from "./harness.ts";

const layouts = ["Stack", "Overlay"] as const;
const states = [
  ["live", false],
  ["frozen", true],
] as const;
// Every track of the mock ticked, or this instance's track shown alone.
const groups = [
  ["several tracks", true, ["Kick", "Bass", "Track 3", "Kick 2"]],
  ["one track", false, ["Kick"]],
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

function matchReadouts(): number {
  return byId("lane-labels").querySelectorAll("[data-help='match']").length;
}

function labelsRow(label: "Off" | "On"): HTMLButtonElement {
  return choice("lane-labels-choice", label);
}

function viewsName(): string {
  return byId("views-details").getAttribute("aria-label") ?? "";
}

async function setLabels(on: boolean): Promise<void> {
  const button = labelsRow(on ? "On" : "Off");
  await press(button);
  await expect.element(button).toHaveAttribute("aria-pressed", "true");
}

// Multi, Trig, with every other track shown or none, in a layout, Compare off, its labels shown;
// then frozen if asked.
async function multi(
  others: boolean,
  layout: string,
  frozen: boolean,
): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Multi"));
  if (!others) {
    await showOnly([]);
  }
  await setCompare(false);
  await press(choice("layout", layout));
  await settle();
  await expect.poll(labelCount).toBeGreaterThan(0);
  if (frozen) {
    await userEvent.click(byId("freeze"));
    // The frozen frame can take longer than the matcher's default second in a busy browser.
    await expect.poll(() => freezeLabel(), { timeout: 5000 }).toBe("Frozen");
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

      test("with Compare on, Off hides the tracks' labels and readouts and keeps the lane Compare adds", async () => {
        await startApp();
        await multi(group, layout, frozen);
        await setCompare(true);
        if (names.length > 1) {
          await expect.poll(matchReadouts).toBeGreaterThan(0);
          await expect
            .poll(() => shownNames().at(-1)?.startsWith("Sum"))
            .toBe(true);
        }
        // The row is a choice like any other, Compare on or off.
        expect(labelsRow("Off").disabled).toBe(false);
        await setLabels(false);
        if (names.length > 1) {
          await expect
            .poll(() => shownNames().map((name) => name.slice(0, 3)))
            .toEqual(["Sum"]);
          expect(matchReadouts()).toBe(0);
          const sum = byId("lane-labels").querySelector(".lane-label > span");
          expect(sum?.querySelector("[data-help='sumGain']")).not.toBeNull();
          expect(sum?.querySelector(".preview-fix")).not.toBeNull();
        } else {
          // Nothing is compared: no label, and the note that says why stays.
          await expect.poll(labelCount).toBe(0);
          expect(
            byId("lane-labels").querySelector(".compare-note")?.textContent,
          ).toContain("No other track to compare with");
        }
        await setLabels(true);
        await expect
          .poll(() => shownNames().slice(0, names.length))
          .toHaveLength(names.length);
        if (names.length > 1) {
          await expect.poll(matchReadouts).toBeGreaterThan(0);
        }

        await setCompare(false);
        await expect.poll(shownNames).toEqual(names);
      });
    });
  });
});

describe("the Labels row", () => {
  test("is in the Layout popover, under Traces, and asks the engine for the choice", async () => {
    const app = await startApp();
    await press(choice("display-mode", "Multi"));
    expect(byId("layout-popover").contains(labelsRow("Off"))).toBe(true);
    await press(labelsRow("Off"));
    const sent = app.sent.filter((message) => message.type === "setSetting");
    expect(sent.at(-1)).toEqual({
      type: "setSetting",
      name: "laneLabels",
      value: "off",
    });
  });

  // One setting for both modes: in Single the labels name the views drawn, in their order and
  // layout, and Off hides them there too, whoever chooses it.
  test.each(layouts)(
    "names Single's views, %s, and hides them when Off",
    async (layout) => {
      const app = await startApp();
      await press(choice("display-mode", "Single"));
      await press(choice("layout", layout));
      await showViews(["L", "R", "S"]);
      expect(
        labelsRow("Off").closest<HTMLElement>(".popover-row")?.hidden,
      ).toBe(false);
      await expect.poll(shownNames).toEqual(["Left", "Right", "Side"]);
      // One label per stacked lane, or one legend.
      expect(labelCount()).toBe(layout === "Stack" ? 3 : 1);
      // The views drawn change: the labels follow the frame.
      await showViews(["L", "M"]);
      await expect.poll(shownNames).toEqual(["Left", "Mid"]);
      await setLabels(false);
      await expect.poll(labelCount).toBe(0);
      // Chosen by the host (automation, an undo), it shows at once.
      app.transport.send({
        type: "setSetting",
        name: "laneLabels",
        value: "on",
      });
      await expect.poll(shownNames).toEqual(["Left", "Mid"]);
      // And it is Multi's choice too.
      await press(choice("display-mode", "Multi"));
      await expect.poll(labelCount).toBeGreaterThan(0);
    },
  );

  test("names a mono track's one view", async () => {
    await startApp({ inputChannels: 1 });
    await settle();
    await expect.poll(shownNames).toEqual(["Left"]);
  });

  // The Layout field shows the layout alone, as every toolbar field shows one value: whether the
  // labels show is seen on the screen, and chosen in the popover. Neither field moves or says
  // anything of the labels when they change.
  test("leaves the Layout and Views fields as they are when Labels changes", async () => {
    await startApp();
    await press(choice("display-mode", "Multi"));
    await press(choice("layout", "Stack"));
    const fields = () =>
      ["layout-details", "views-details"].map((id) => {
        const { left, width } = byId(id).getBoundingClientRect();
        return {
          left,
          width,
          name: byId(id).getAttribute("aria-label"),
          text: byId(id).querySelector("b .on")?.textContent,
        };
      });
    await setLabels(true);
    const on = fields();
    expect(on[0]?.name).toBe("Layout: Stack");
    expect(on[0]?.text).toBe("Stack");
    expect(viewsName()).toBe("Views: 4 of 4 tracks, Mid");
    await setLabels(false);
    expect(fields()).toEqual(on);
  });

  test("is the engine's: the page reopened shows the labels off", async () => {
    const first: Started = await startApp();
    await multi(true, "Stack", false);
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
    await multi(true, "Stack", false);
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
