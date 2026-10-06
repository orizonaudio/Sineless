// The settings table of DESIGN 1.2. Times are in seconds. The engine validates against these
// ranges, and the UI builds its controls from them.
import {
  BeatSpan,
  ChannelView,
  TimeBase,
  TriggerMode,
  TriggerSlope,
  ViewLayout,
  type EnumName,
} from "./enums.ts";

export interface NumberSetting {
  readonly kind: "number";
  readonly minimum: number;
  readonly maximum: number;
  readonly defaultValue: number;
  readonly logarithmic: boolean;
}

// One value of the named enumeration.
export interface EnumSetting {
  readonly kind: "enum";
  readonly values: EnumName;
  readonly defaultValue: number;
}

// A non-empty set of values of the named enumeration, as a bit mask (bit n = value n).
export interface FlagsSetting {
  readonly kind: "flags";
  readonly values: EnumName;
  readonly defaultValue: number;
}

export type Setting = NumberSetting | EnumSetting | FlagsSetting;

const bit = (value: number): number => 1 << value;

export const settings = {
  timeSpan: {
    kind: "number",
    minimum: 0.0005,
    maximum: 4,
    defaultValue: 0.02,
    logarithmic: true,
  },
  verticalScale: {
    kind: "number",
    minimum: -60,
    maximum: 6,
    defaultValue: 0,
    logarithmic: false,
  },
  channelViews: {
    kind: "flags",
    values: "ChannelView",
    defaultValue: bit(ChannelView.left) | bit(ChannelView.right),
  },
  viewLayout: {
    kind: "enum",
    values: "ViewLayout",
    defaultValue: ViewLayout.overlaid,
  },
  triggerMode: {
    kind: "enum",
    values: "TriggerMode",
    defaultValue: TriggerMode.auto,
  },
  triggerSource: {
    kind: "enum",
    values: "ChannelView",
    defaultValue: ChannelView.mid,
  },
  triggerSlope: {
    kind: "enum",
    values: "TriggerSlope",
    defaultValue: TriggerSlope.rising,
  },
  triggerLevel: {
    kind: "number",
    minimum: -1,
    maximum: 1,
    defaultValue: 0,
    logarithmic: false,
  },
  triggerHysteresis: {
    kind: "number",
    minimum: 0,
    maximum: 0.5,
    defaultValue: 0.01,
    logarithmic: false,
  },
  triggerHoldOff: {
    kind: "number",
    minimum: 0,
    maximum: 1,
    defaultValue: 0,
    logarithmic: false,
  },
  timeBase: {
    kind: "enum",
    values: "TimeBase",
    defaultValue: TimeBase.seconds,
  },
  beatSpan: { kind: "enum", values: "BeatSpan", defaultValue: BeatSpan.oneBar },
} as const satisfies Record<string, Setting>;

export type SettingName = keyof typeof settings;
