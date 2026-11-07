// Which grid the screen draws: the Type, Time and Level settings of the time base shown, and the
// one Beat division. The page draws and labels with it and sends it to the macOS editor in its
// trace view, so both draw the same grid.
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

export type GridSettingKind = "Type" | "Time" | "Level";

const timeBaseSuffix = {
  trigger: "Trigger",
  beats: "Beats",
  free: "Free",
} as const;

// The setting holding a grid choice for a time base, e.g. gridTimeFree.
export function gridSettingName(
  kind: GridSettingKind,
  timeBase: TimeBaseName,
): SettingName {
  return `grid${kind}${timeBaseSuffix[timeBase]}`;
}

// Whether a setting is one the grid is drawn from.
export function isGridSetting(name: SettingName): boolean {
  return name.startsWith("grid") || name === "beatDivision";
}

// Cross hair marks the trigger point, which only Trig has: a project holding it for another time
// base (edited by hand) draws Grid there.
export function effectiveGridType(
  type: GridTypeName,
  timeBase: TimeBaseName,
): GridTypeName {
  return type === "crossHair" && timeBase !== "trigger" ? "grid" : type;
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

// The grid drawn in `timeBase`, from the store's values.
export function gridChoice(
  get: (name: SettingName) => SettingValue,
  timeBase: TimeBaseName,
): GridChoice {
  const type = nameOf(GridType, get(gridSettingName("Type", timeBase)), "grid");
  return {
    type: effectiveGridType(type, timeBase),
    time: nameOf(GridDensity, get(gridSettingName("Time", timeBase)), "medium"),
    level: nameOf(
      GridDensity,
      get(gridSettingName("Level", timeBase)),
      "medium",
    ),
    division: nameOf(BeatDivision, get("beatDivision"), "regular"),
  };
}
