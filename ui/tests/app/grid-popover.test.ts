// The Grid field in the status bar and its popover (#364): each row chooses the shown time base's
// setting (Beat division its one setting), the field names the type drawn and keeps its width, each
// time base keeps its own choices, and the rows that do not apply are disabled: Cross hair outside
// Trig, Time and Level with None, Beat division outside Beats.
import { describe, expect, test } from "vitest";

import {
  BeatDivision,
  GridDensity,
  GridType,
  TimeBase,
} from "../../../protocol/src/enums.ts";
import { choiceLabels } from "../../src/view/choice-labels.ts";
import {
  gridSettingName,
  type TimeBaseName,
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

// Every row's choices, each with the setting it writes in a time base.
const rows = [
  ...Object.keys(GridType).map((value) => ({
    row: "grid-type",
    value,
    label: choiceLabels.gridType[value as keyof typeof GridType],
    setting: (timeBase: TimeBaseName) => gridSettingName("Type", timeBase),
  })),
  ...(["Time", "Level"] as const).flatMap((kind) =>
    Object.keys(GridDensity).map((value) => ({
      row: kind === "Time" ? "grid-time" : "grid-level",
      value,
      label: choiceLabels.gridDensity[value as keyof typeof GridDensity],
      setting: (timeBase: TimeBaseName) => gridSettingName(kind, timeBase),
    })),
  ),
  ...Object.keys(BeatDivision).map((value) => ({
    row: "beat-division",
    value,
    label: choiceLabels.beatDivision[value as keyof typeof BeatDivision],
    setting: () => "beatDivision",
  })),
];

// The choices a user can make in each time base: Cross hair in Trig only, Beat division in Beats.
const cases = timeBases.flatMap((timeBase) =>
  rows
    .filter(
      (entry) =>
        (entry.value !== "crossHair" || timeBase === "trigger") &&
        (entry.row !== "beat-division" || timeBase === "beats"),
    )
    .map((entry) => ({ ...entry, timeBase, base: timeBaseLabel[timeBase] })),
);

function summary(): string {
  return byId("grid-summary").querySelector(".on")?.textContent ?? "";
}

describe("the Grid popover", () => {
  test.each(cases)(
    "in $base, $label in $row sets $timeBase's own setting",
    async ({ row, value, label, setting, timeBase, base }) => {
      const app = await startApp();
      await inTimeBase(base);
      await press(choice(row, label));
      await settle();
      const sent = app.sent.filter((message) => message.type === "setSetting");
      expect(sent.at(-1)).toEqual({
        type: "setSetting",
        name: setting(timeBase),
        value,
      });
      expect(choice(row, label).getAttribute("aria-pressed")).toBe("true");
      // A choice keeps the popover open.
      expect(byId("grid-popover").hidden).toBe(false);
    },
  );

  test("each time base keeps its own type, as the field shows", async () => {
    await startApp();
    await inTimeBase("Trig");
    expect(summary()).toBe("Grid");
    await press(choice("grid-type", "None"));
    await settle();
    expect(summary()).toBe("None");
    await press(choice("grid-type", "Cross hair"));
    await settle();
    expect(summary()).toBe("Cross hair");
    await inTimeBase("Beats");
    await settle();
    expect(summary()).toBe("Grid");
    await press(choice("grid-type", "None"));
    await settle();
    await inTimeBase("Free");
    await settle();
    expect(summary()).toBe("Grid");
    await inTimeBase("Trig");
    await settle();
    expect(summary()).toBe("Cross hair");
    await inTimeBase("Beats");
    await settle();
    expect(summary()).toBe("None");
  });

  test.each(timeBases)(
    "in %s, only the choices that apply are enabled",
    async (timeBase) => {
      await startApp();
      await inTimeBase(timeBaseLabel[timeBase]);
      expect(choice("grid-type", "Cross hair").disabled).toBe(
        timeBase !== "trigger",
      );
      expect(choice("grid-type", "None").disabled).toBe(false);
      for (const label of ["Regular", "Triplets"]) {
        expect(choice("beat-division", label).disabled).toBe(
          timeBase !== "beats",
        );
      }
      const densityRows = ["grid-time", "grid-level"];
      for (const row of densityRows) {
        expect(choice(row, "Fine").disabled).toBe(false);
      }
      await press(choice("grid-type", "None"));
      await settle();
      for (const row of densityRows) {
        for (const label of ["Coarse", "Medium", "Fine"]) {
          expect(choice(row, label).disabled, `${row} ${label}`).toBe(true);
        }
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
    for (const label of ["None", "Cross hair", "Grid"]) {
      await press(choice("grid-type", label));
      await settle();
      expect(box(), label).toEqual(first);
    }
  });
});
