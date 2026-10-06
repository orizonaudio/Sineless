import { describe, expect, test } from "vitest";

import {
  defaultSettingValues,
  isValidSettingValue,
  toSettingValue,
} from "../src/setting-values.ts";

describe("setting values", () => {
  test("defaults follow the settings table", () => {
    const defaults = defaultSettingValues();
    expect(defaults.timeSpan).toBe(0.02);
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
