// Which grid the screen draws: one Type and Density for every time base, and the one Beat
// division. The Density sets both the time step and the level step. The page draws and labels with
// it and sends it to the macOS editor in its trace view, so both draw the same grid.
import {
  BeatDivision,
  GridDensity,
  GridType,
} from "../../../protocol/src/enums.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";

export type TimeBaseName = "trigger" | "beats" | "free";
export type GridTypeName = keyof typeof GridType;
export type GridDensityName = keyof typeof GridDensity;
export type BeatDivisionName = keyof typeof BeatDivision;

export interface GridChoice {
  readonly type: GridTypeName;
  readonly time: GridDensityName;
  readonly level: GridDensityName;
  readonly division: BeatDivisionName;
}

export type GridSettingKind = "Type" | "Density";

// The setting holding a grid choice, e.g. gridDensity.
export function gridSettingName(kind: GridSettingKind): SettingName {
  return `grid${kind}`;
}

// Whether a setting is one the grid is drawn from.
export function isGridSetting(name: SettingName): boolean {
  return name.startsWith("grid") || name === "beatDivision";
}

// Each type adds to the one before (GridType): the zero lines, then the labels, then the lines.
// Whether a type draws the time and level lines.
export function drawsLines(type: GridTypeName): boolean {
  return GridType[type] >= GridType.grid;
}

// Whether a type draws the time and level labels.
export function drawsLabels(type: GridTypeName): boolean {
  return GridType[type] >= GridType.ticks;
}

function nameOf<T extends Record<string, number>>(
  values: T,
  value: SettingValue,
  fallback: keyof T & string,
): keyof T & string {
  return typeof value === "string" && Object.hasOwn(values, value)
    ? value
    : fallback;
}

// The grid drawn, from the store's values.
export function gridChoice(
  get: (name: SettingName) => SettingValue,
): GridChoice {
  const density = nameOf(GridDensity, get("gridDensity"), "medium");
  return {
    type: nameOf(GridType, get("gridType"), "grid"),
    time: density,
    level: density,
    division: nameOf(BeatDivision, get("beatDivision"), "regular"),
  };
}
