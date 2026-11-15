// The Grid field in the status bar and its popover (#364, #384): each row chooses its one setting,
// in force in every time base, the field names the type drawn and keeps its width, and the rows
// that do not apply are disabled: Density and Beat division with None, Beat division outside
// Beats. Its last row is A4 (#425), the concert pitch that tunes the note names the readouts
// show: it applies whatever the grid, so it is never disabled.
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import {
  BeatDivision,
  GridDensity,
  GridType,
  TimeBase,
} from "../../../protocol/src/enums.ts";
import { choiceLabels } from "../../src/view/choice-labels.ts";
import { helpTexts } from "../../src/view/help.ts";
import type {
  GridTypeName,
  TimeBaseName,
} from "../../src/view/grid-settings.ts";
import {
  byId,
  choice,
  cursorRow,
  inTimeBase,
  press,
  setPressed,
  settle,
  showDisplay,
  startApp,
  typeInto,
} from "./harness.ts";

const timeBaseLabel: Readonly<Record<TimeBaseName, string>> = {
  trigger: "Trig",
  beats: "Beats",
  free: "Free",
};
const timeBases = Object.keys(TimeBase) as TimeBaseName[];
const types = Object.keys(GridType) as GridTypeName[];

// Every row's choices, each with the setting it writes.
const rows = [
  ...types.map((value) => ({
    row: "grid-type",
    value,
    label: choiceLabels.gridType[value],
    setting: "gridType",
  })),
  ...Object.keys(GridDensity).map((value) => ({
    row: "grid-density",
    value,
    label: choiceLabels.gridDensity[value as keyof typeof GridDensity],
    setting: "gridDensity",
  })),
  ...Object.keys(BeatDivision).map((value) => ({
    row: "beat-division",
    value,
    label: choiceLabels.beatDivision[value as keyof typeof BeatDivision],
    setting: "beatDivision",
  })),
];

// The choices a user can make in each time base: Beat division in Beats only.
const cases = timeBases.flatMap((timeBase) =>
  rows
    .filter((entry) => entry.row !== "beat-division" || timeBase === "beats")
    .map((entry) => ({ ...entry, base: timeBaseLabel[timeBase] })),
);

function summary(): string {
  return byId("grid-summary").querySelector(".on")?.textContent ?? "";
}

describe("the Grid popover", () => {
  test.each(cases)(
    "in $base, $label in $row sets $setting",
    async ({ row, value, label, setting, base }) => {
      const app = await startApp();
      await inTimeBase(base);
      await press(choice(row, label));
      await settle();
      const sent = app.sent.filter((message) => message.type === "setSetting");
      expect(sent.at(-1)).toEqual({ type: "setSetting", name: setting, value });
      expect(choice(row, label).getAttribute("aria-pressed")).toBe("true");
      // A choice keeps the popover open.
      expect(byId("grid-popover").hidden).toBe(false);
    },
  );

  test("a type chosen in one time base is the type in every other, as the field shows", async () => {
    await startApp();
    await inTimeBase("Trig");
    expect(summary()).toBe("Grid");
    await press(choice("grid-type", "None"));
    await settle();
    expect(summary()).toBe("None");
    for (const base of ["Beats", "Free", "Trig"]) {
      await inTimeBase(base);
      await settle();
      expect(summary(), base).toBe("None");
    }
    await inTimeBase("Free");
    await press(choice("grid-type", "Ticks"));
    await settle();
    for (const base of ["Beats", "Trig", "Free"]) {
      await inTimeBase(base);
      await settle();
      expect(summary(), base).toBe("Ticks");
    }
  });

  // Density and (in Beats) Beat division set where the lines and the labels go: greyed out under
  // None, which draws neither; they keep their choice for when another type is chosen.
  test.each(
    timeBases.flatMap((timeBase) =>
      types.map((type) => ({ timeBase, type, base: timeBaseLabel[timeBase] })),
    ),
  )(
    "in $base with $type, only the choices that apply are enabled",
    async ({ timeBase, type, base }) => {
      await startApp();
      await inTimeBase(base);
      await press(choice("grid-type", choiceLabels.gridType[type]));
      await settle();
      for (const other of types) {
        expect(choice("grid-type", choiceLabels.gridType[other]).disabled).toBe(
          false,
        );
      }
      const steps = type !== "none";
      for (const label of Object.values(choiceLabels.gridDensity)) {
        expect(choice("grid-density", label).disabled, label).toBe(!steps);
      }
      // The choice stays lit, greyed out or not.
      expect(
        choice("grid-density", choiceLabels.gridDensity.medium).getAttribute(
          "aria-pressed",
        ),
      ).toBe("true");
      for (const label of Object.values(choiceLabels.beatDivision)) {
        expect(choice("beat-division", label).disabled, label).toBe(
          !steps || timeBase !== "beats",
        );
      }
    },
  );

  test("the field keeps its width and place whatever the type", async () => {
    await startApp();
    await inTimeBase("Trig");
    const box = () => {
      const { left, width } = byId("grid-details").getBoundingClientRect();
      return { left, width };
    };
    const first = box();
    for (const label of Object.values(choiceLabels.gridType)) {
      await press(choice("grid-type", label));
      await settle();
      expect(box(), label).toEqual(first);
    }
  });
});

async function openGrid(): Promise<void> {
  if (byId("grid-popover").hidden) {
    await userEvent.click(byId("grid-details"));
  }
  await expect.element(byId("grid-popover")).toBeVisible();
}

describe("A4 in the Grid popover", () => {
  test("is the last row, after Beat division", async () => {
    await startApp();
    await openGrid();
    const rows = [
      ...byId("grid-popover").querySelectorAll<HTMLElement>(".popover-row"),
    ].map((row) => row.querySelector("span")?.textContent ?? "");
    expect(rows).toEqual(["Type", "Density", "Beat division", "A4"]);
    expect(byId("grid-popover").contains(byId("concert-pitch-field"))).toBe(
      true,
    );
  });

  // Note names are read from the cursors and the Meter with no grid drawn too.
  test.each(types)("can be set with the grid type %s", async (type) => {
    const app = await startApp();
    await press(choice("grid-type", choiceLabels.gridType[type]));
    await settle();
    await typeInto("concert-pitch-field", "432");
    expect(
      app.sent.filter((message) => message.type === "setSetting").at(-1),
    ).toEqual({ type: "setSetting", name: "concertPitch", value: 432 });
  });

  test.each(["Single", "Multi"] as const)(
    "tunes the time cursors' note name in %s",
    async (mode) => {
      await startApp();
      await showDisplay(mode);
      // A 10 ms span in Trig puts the time cursors a third of it apart, at 300 Hz: audible, so
      // named.
      await inTimeBase("Trig");
      await typeInto("span-field", "10 ms");
      await settle();
      await setPressed(byId("time-cursors"), true);
      await expect.poll(() => cursorRow("Note")).not.toBe("");
      const at440 = cursorRow("Note");
      await openGrid();
      await typeInto("concert-pitch-field", "432");
      // A lower A4 names the same frequency sharper.
      await expect.poll(() => cursorRow("Note")).not.toBe(at440);
      await typeInto("concert-pitch-field", "440");
      await expect.poll(() => cursorRow("Note")).toBe(at440);
    },
  );

  test("is explained by concert pitch's help", async () => {
    await startApp();
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
    await openGrid();
    const box = byId("concert-pitch-field").getBoundingClientRect();
    byId("concert-pitch-field").dispatchEvent(
      new PointerEvent("pointermove", {
        pointerId: 1,
        bubbles: true,
        clientX: box.left + box.width / 2,
        clientY: box.top + box.height / 2,
      }),
    );
    await vi.waitFor(
      () => {
        expect(byId("help-tip").textContent).toContain(
          helpTexts.concertPitch.lead,
        );
      },
      { timeout: 5000 },
    );
  });
});
