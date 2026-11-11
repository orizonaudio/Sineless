// Which grid the screen draws: one Type and Density for every time base, and the one Beat
// division; and what each type draws.
import { expect, test } from "vitest";

import { GridDensity, GridType } from "../../protocol/src/enums.ts";
import { defaultSettingValues } from "../../protocol/src/setting-values.ts";
import type { SettingName } from "../../protocol/src/settings.ts";
import {
  drawsLabels,
  drawsLines,
  gridChoice,
  isGridSetting,
  type GridDensityName,
  type GridTypeName,
} from "../src/view/grid-settings.ts";

const types = Object.keys(GridType) as GridTypeName[];
const densities = Object.keys(GridDensity) as GridDensityName[];

// What each type draws, from the issue (#384): each adds to the one before.
const draws: Readonly<
  Record<GridTypeName, { readonly lines: boolean; readonly labels: boolean }>
> = {
  none: { lines: false, labels: false },
  ticks: { lines: false, labels: true },
  grid: { lines: true, labels: true },
};

test("a new instance draws the full grid at Medium", () => {
  const values = defaultSettingValues();
  expect(gridChoice((name) => values[name])).toEqual({
    type: "grid",
    time: "medium",
    level: "medium",
    division: "regular",
  });
});

test("the grid drawn is the one chosen, whatever the time base", () => {
  const values = defaultSettingValues();
  values.gridType = "ticks";
  values.gridDensity = "coarse";
  values.beatDivision = "triplets";
  for (const timeBase of ["trigger", "beats", "free"]) {
    values.timeBase = timeBase;
    expect(
      gridChoice((name) => values[name]),
      timeBase,
    ).toEqual({
      type: "ticks",
      time: "coarse",
      level: "coarse",
      division: "triplets",
    });
  }
});

test.each(densities)(
  "the %s Density sets both the time and the level step",
  (density) => {
    const values = defaultSettingValues();
    values.gridDensity = density;
    const grid = gridChoice((name) => values[name]);
    expect(grid.time).toBe(density);
    expect(grid.level).toBe(density);
  },
);

test.each([undefined, "huge", 3])(
  "a Density the store cannot name (%s) draws both steps at Medium",
  (value) => {
    const values: Record<string, unknown> = defaultSettingValues();
    values["gridDensity"] = value;
    const grid = gridChoice((name) => values[name] as never);
    expect(grid.time).toBe("medium");
    expect(grid.level).toBe("medium");
  },
);

test.each(types)("%s draws its lines and labels as the issue says", (type) => {
  expect(drawsLines(type)).toBe(draws[type].lines);
  expect(drawsLabels(type)).toBe(draws[type].labels);
});

test("every grid setting, and only those, redraw the grid", () => {
  const names = Object.keys(defaultSettingValues()) as SettingName[];
  const grid: SettingName[] = ["gridType", "gridDensity", "beatDivision"];
  for (const name of names) {
    expect(isGridSetting(name), name).toBe(grid.includes(name));
  }
});
