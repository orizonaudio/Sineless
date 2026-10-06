// JSON values of settings (AD-9), mirroring the engine's SettingJson: numbers for number
// settings, value names for enum settings, arrays of names for flags settings.
import { enums, type EnumName } from "./enums.ts";
import type { SettingValue } from "./messages.ts";
import { settings, type Setting, type SettingName } from "./settings.ts";

// Value names of an enumeration, indexed by value.
export function valueNames(name: EnumName): string[] {
  return Object.entries(enums[name])
    .sort(([, a], [, b]) => a - b)
    .map(([key]) => key);
}

export const settingNames = Object.keys(settings) as SettingName[];

export function settingOf(name: SettingName): Setting {
  return settings[name];
}

export function isSettingName(name: unknown): name is SettingName {
  return typeof name === "string" && Object.hasOwn(settings, name);
}

export function toSettingValue(name: SettingName, value: number): SettingValue {
  const setting = settingOf(name);
  switch (setting.kind) {
    case "number":
      return value;
    case "enum":
      return valueNames(setting.values)[value] ?? "";
    case "flags":
      return valueNames(setting.values).filter(
        (_, index) => (value & (1 << index)) !== 0,
      );
  }
}

export function defaultSettingValues(): Record<SettingName, SettingValue> {
  return Object.fromEntries(
    settingNames.map((name) => [
      name,
      toSettingValue(name, settingOf(name).defaultValue),
    ]),
  ) as Record<SettingName, SettingValue>;
}

// Whether the engine would accept `value` for the setting.
export function isValidSettingValue(
  name: SettingName,
  value: unknown,
): value is SettingValue {
  const setting = settingOf(name);
  switch (setting.kind) {
    case "number":
      return (
        typeof value === "number" &&
        Number.isFinite(value) &&
        value >= setting.minimum &&
        value <= setting.maximum
      );
    case "enum":
      return (
        typeof value === "string" && valueNames(setting.values).includes(value)
      );
    case "flags": {
      const names = valueNames(setting.values);
      return (
        Array.isArray(value) &&
        value.length > 0 &&
        value.every(
          (entry) => typeof entry === "string" && names.includes(entry),
        )
      );
    }
  }
}
