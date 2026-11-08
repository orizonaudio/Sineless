// The plugin's settings. Times are in seconds. The engine validates against these
// ranges, and the UI builds its controls from them.
import {
  BeatDivision,
  BeatSpan,
  ChannelView,
  DisplayMode,
  GridDensity,
  GridType,
  Group,
  TimeBase,
  Toggle,
  TriggerMode,
  TriggerSlope,
  ViewLayout,
  type EnumName,
} from "./enums.ts";
import {
  defaultTriggerPosition,
  frozenHoldSeconds,
  longestTriggerSpanSeconds,
  shortestFreeSpanSeconds,
} from "./limits.ts";

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
  // The span of the Trigger time base, and of the Free time base.
  triggerSpan: {
    kind: "number",
    minimum: 0.0005,
    maximum: longestTriggerSpanSeconds,
    defaultValue: 1,
    logarithmic: true,
  },
  freeSpan: {
    kind: "number",
    minimum: shortestFreeSpanSeconds,
    maximum: frozenHoldSeconds,
    defaultValue: 4,
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
    defaultValue: ViewLayout.stacked,
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
  // The Channel: what the Meter, and with the views overlaid the hover and snapping, read in the
  // Single display mode, when it is drawn; the first drawn view otherwise. Multi, with or without
  // Compare, reads this track's lane channel. Saved under its first name.
  readoutSource: {
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
  // Where the trigger point sits, as a fraction of the span from the left (the Trigger time base):
  // 0 shows only what follows the trigger, 1 only what led up to it.
  triggerPosition: {
    kind: "number",
    minimum: 0,
    maximum: 1,
    defaultValue: defaultTriggerPosition,
    logarithmic: false,
  },
  timeBase: {
    kind: "enum",
    values: "TimeBase",
    defaultValue: TimeBase.beats,
  },
  beatSpan: { kind: "enum", values: "BeatSpan", defaultValue: BeatSpan.oneBar },
  // Freeze when the host transport stops, and resume when it plays again. On in a new instance:
  // what was on screen when playback stopped is what a user stops to study.
  autoFreeze: { kind: "enum", values: "Toggle", defaultValue: Toggle.on },
  displayMode: {
    kind: "enum",
    values: "DisplayMode",
    defaultValue: DisplayMode.single,
  },
  group: { kind: "enum", values: "Group", defaultValue: Group.a },
  // In Multi, adds the sum of the tracks checked against this one, with their matches and summing
  // gains (Compare, FR-15). Kept while the display mode is Single.
  compare: { kind: "enum", values: "Toggle", defaultValue: Toggle.off },
  // The channel every lane of Multi shows, with or without Compare, and what Multi's trigger,
  // readouts, sum, matches and summing gains read: one choice for the whole display. A mono track
  // shows its one channel whatever the choice. Mid keeps what Multi showed before the choice
  // existed. Chosen while frozen, this track's lane is redrawn from its hold and the other
  // tracks' lanes are empty until the display resumes: their audio is held as one channel.
  laneChannel: {
    kind: "enum",
    values: "ChannelView",
    defaultValue: ChannelView.mid,
  },
  // The readout beside the pointer over the traces.
  hoverReadout: { kind: "enum", values: "Toggle", defaultValue: Toggle.off },
  // The Meter in the screen's corner: Peak, RMS and their crest factor, over the whole span.
  meter: { kind: "enum", values: "Toggle", defaultValue: Toggle.off },
  // The pitch of A4 that note names refer to, in Hz.
  concertPitch: {
    kind: "number",
    minimum: 400,
    maximum: 500,
    defaultValue: 440,
    logarithmic: false,
  },
  // The screen's grid, for each time base: what it draws, and how closely it divides time and
  // level.
  gridTypeBeats: {
    kind: "enum",
    values: "GridType",
    defaultValue: GridType.grid,
  },
  gridTypeFree: {
    kind: "enum",
    values: "GridType",
    defaultValue: GridType.grid,
  },
  gridTypeTrigger: {
    kind: "enum",
    values: "GridType",
    defaultValue: GridType.grid,
  },
  gridTimeBeats: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  gridTimeFree: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  gridTimeTrigger: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  gridLevelBeats: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  gridLevelFree: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  gridLevelTrigger: {
    kind: "enum",
    values: "GridDensity",
    defaultValue: GridDensity.medium,
  },
  // Beats' grid below the beat, in every Beats view.
  beatDivision: {
    kind: "enum",
    values: "BeatDivision",
    defaultValue: BeatDivision.regular,
  },
} as const satisfies Record<string, Setting>;

export type SettingName = keyof typeof settings;
