import { describe, expect, test } from "vitest";

import {
  defaultSettingValues,
  isValidSettingValue,
  settingNames,
  settingOf,
  toSettingValue,
  valueNames,
} from "../src/setting-values.ts";

describe("setting values", () => {
  test("defaults follow the settings table", () => {
    const defaults = defaultSettingValues();
    expect(defaults.timeSpan).toBe(1);
    expect(defaults.channelViews).toEqual(["left", "right"]);
    expect(defaults.triggerMode).toBe("auto");
    expect(defaults.triggerSource).toBe("mid");
  });

  test("converts numbers to names for enums and flags", () => {
    expect(toSettingValue("triggerSlope", 1)).toBe("falling");
    expect(toSettingValue("channelViews", 0b1100)).toEqual(["mid", "side"]);
  });

  test("validates like the engine", () => {
    expect(isValidSettingValue("timeSpan", 4)).toBe(true);
    expect(isValidSettingValue("timeSpan", 4.5)).toBe(false);
    expect(isValidSettingValue("timeSpan", Number.NaN)).toBe(false);
    expect(isValidSettingValue("timeSpan", "0.5")).toBe(false);
    expect(isValidSettingValue("triggerMode", "single")).toBe(true);
    expect(isValidSettingValue("triggerMode", "sometimes")).toBe(false);
    expect(isValidSettingValue("channelViews", [])).toBe(false);
    expect(isValidSettingValue("channelViews", ["left", "up"])).toBe(false);
    expect(isValidSettingValue("channelViews", ["side"])).toBe(true);
  });
});

// Every value worth trying for each setting in the table: a number's minimum, maximum and
// default; every enum value; each flag alone and all of them. The engine tests the same list
// (StateTests), so the two sides cannot accept different values.
const candidates = settingNames.flatMap((name) => {
  const setting = settingOf(name);
  const values =
    setting.kind === "number"
      ? [setting.minimum, setting.maximum, setting.defaultValue]
      : setting.kind === "enum"
        ? valueNames(setting.values).map((_, index) => index)
        : [
            ...valueNames(setting.values).map((_, bit) => 1 << bit),
            (1 << valueNames(setting.values).length) - 1,
          ];
  return values.map((value) => ({ name, value }));
});

describe.each(candidates)("$name = $value", ({ name, value }) => {
  test("converts to a value the engine accepts", () => {
    expect(isValidSettingValue(name, toSettingValue(name, value))).toBe(true);
  });
});
