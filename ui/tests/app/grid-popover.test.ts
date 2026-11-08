// The Grid field in the status bar and its popover (#364, #384): each row chooses its one setting,
// in force in every time base, the field names the type drawn and keeps its width, and the rows
// that do not apply are disabled: Time, Level and Beat division with None, Beat division outside
// Beats.
import { describe, expect, test } from "vitest";

import {
  BeatDivision,
  GridDensity,
  GridType,
  TimeBase,
} from "../../../protocol/src/enums.ts";
import { choiceLabels } from "../../src/view/choice-labels.ts";
import type {
  GridTypeName,
  TimeBaseName,
} from "../../src/view/grid-settings.ts";
import {
  byId,
  choice,
  inTimeBase,
  press,
  settle,
  startApp,
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
  ...(["Time", "Level"] as const).flatMap((kind) =>
    Object.keys(GridDensity).map((value) => ({
      row: kind === "Time" ? "grid-time" : "grid-level",
      value,
      label: choiceLabels.gridDensity[value as keyof typeof GridDensity],
      setting: `grid${kind}`,
    })),
  ),
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

  // Time, Level and (in Beats) Beat division set where the lines and the labels go: greyed out
  // under None, which draws neither; they keep their choice for when another type is chosen.
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
      for (const row of ["grid-time", "grid-level"]) {
        for (const label of Object.values(choiceLabels.gridDensity)) {
          expect(choice(row, label).disabled, `${row} ${label}`).toBe(!steps);
        }
      }
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
