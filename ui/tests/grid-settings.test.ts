// Which grid the screen draws: the shown time base's Type, Time and Level, and the one Beat
// division.
import { expect, test } from "vitest";

import { GridType, TimeBase } from "../../protocol/src/enums.ts";
import type { SettingValue } from "../../protocol/src/messages.ts";
import { defaultSettingValues } from "../../protocol/src/setting-values.ts";
import type { SettingName } from "../../protocol/src/settings.ts";
import {
  gridChoice,
  gridSettingName,
  isGridSetting,
  type GridTypeName,
  type TimeBaseName,
} from "../src/view/grid-settings.ts";

const timeBases = Object.keys(TimeBase) as TimeBaseName[];
const types = Object.keys(GridType) as GridTypeName[];

test("a new instance draws today's grid in every time base", () => {
  const values = defaultSettingValues();
  for (const timeBase of timeBases) {
    expect(gridChoice((name) => values[name], timeBase)).toEqual({
      type: "grid",
      time: "medium",
      level: "medium",
      division: "regular",
    });
  }
});

test.each(timeBases)("in %s the grid is that time base's own", (timeBase) => {
  const values: Record<SettingName, SettingValue> = defaultSettingValues();
  // Every other time base away from the defaults: none of it may show here.
  for (const other of timeBases.filter((base) => base !== timeBase)) {
    values[gridSettingName("Type", other)] = "none";
    values[gridSettingName("Time", other)] = "fine";
    values[gridSettingName("Level", other)] = "fine";
  }
  values[gridSettingName("Time", timeBase)] = "coarse";
  values[gridSettingName("Level", timeBase)] = "fine";
  values.beatDivision = "triplets";
  expect(gridChoice((name) => values[name], timeBase)).toEqual({
    type: "grid",
    time: "coarse",
    level: "fine",
    division: "triplets",
  });
});

test.each(
  timeBases.flatMap((timeBase) =>
    types.map((type) => [timeBase, type] as const),
  ),
)("in %s, %s draws as chosen", (timeBase, type) => {
  const values = defaultSettingValues();
  values[gridSettingName("Type", timeBase)] = type;
  expect(gridChoice((name) => values[name], timeBase).type).toBe(type);
});

test("every grid setting, and only those, redraw the grid", () => {
  const names = Object.keys(defaultSettingValues()) as SettingName[];
  const grid = timeBases.flatMap((timeBase) =>
    (["Type", "Time", "Level"] as const).map((kind) =>
      gridSettingName(kind, timeBase),
    ),
  );
  for (const name of names) {
    expect(isGridSetting(name), name).toBe(
      grid.includes(name) || name === "beatDivision",
    );
  }
});
