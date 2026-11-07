// Enumerations shared by the engine and the UI. Names are stable: projects save them; the numbers
// may change.

export const ChannelView = { left: 0, right: 1, mid: 2, side: 3 } as const;
export type ChannelView = (typeof ChannelView)[keyof typeof ChannelView];

export const ViewLayout = { overlaid: 0, stacked: 1 } as const;
export type ViewLayout = (typeof ViewLayout)[keyof typeof ViewLayout];

export const TriggerMode = { auto: 0, normal: 1, single: 2 } as const;
export type TriggerMode = (typeof TriggerMode)[keyof typeof TriggerMode];

export const TriggerSlope = { rising: 0, falling: 1 } as const;
export type TriggerSlope = (typeof TriggerSlope)[keyof typeof TriggerSlope];

// Trigger: a fixed span placed by the trigger. Beats: a sweep in time with the host. Free: a fixed
// span ending at the newest audio, moving with it.
export const TimeBase = { trigger: 0, beats: 1, free: 2 } as const;
export type TimeBase = (typeof TimeBase)[keyof typeof TimeBase];

// Tempo-synced spans, in bars of the host time signature: fractions of a bar, then bars.
export const BeatSpan = {
  eighthBar: 0,
  sixthBar: 1,
  quarterBar: 2,
  thirdBar: 3,
  halfBar: 4,
  oneBar: 5,
  twoBars: 6,
  threeBars: 7,
  fourBars: 8,
  sixBars: 9,
  eightBars: 10,
  sixteenBars: 11,
} as const;
export type BeatSpan = (typeof BeatSpan)[keyof typeof BeatSpan];

// Length of each beat span, in bars of the host time signature. Indexed by BeatSpan value.
export const beatSpanLengths: readonly {
  readonly amount: number;
}[] = [
  { amount: 1 / 8 },
  { amount: 1 / 6 },
  { amount: 1 / 4 },
  { amount: 1 / 3 },
  { amount: 1 / 2 },
  { amount: 1 },
  { amount: 2 },
  { amount: 3 },
  { amount: 4 },
  { amount: 6 },
  { amount: 8 },
  { amount: 16 },
];

export function beatSpanQuarterNotes(
  span: BeatSpan,
  barQuarterNotes: number,
): number {
  const length = beatSpanLengths[span] ?? { amount: 1 };
  return length.amount * barQuarterNotes;
}

export const Toggle = { off: 0, on: 1 } as const;
export type Toggle = (typeof Toggle)[keyof typeof Toggle];

// Single shows this track's channels; Multi shows every track of the instance's group, and with
// the compare setting on, the sum of those checked against this track.
export const DisplayMode = { single: 0, multi: 1 } as const;
export type DisplayMode = (typeof DisplayMode)[keyof typeof DisplayMode];

// Instances in the same group appear together in the multi-track view.
export const Group = { a: 0, b: 1, c: 2, d: 3 } as const;
export type Group = (typeof Group)[keyof typeof Group];

export const FrameMode = { minMax: 0, raw: 1 } as const;
export type FrameMode = (typeof FrameMode)[keyof typeof FrameMode];

// What the screen's grid draws in a time base: nothing but each lane's zero line; the full grid; or
// the cross hair, the zero lines and the trigger point's vertical line with ticks, in Trig only.
export const GridType = { none: 0, grid: 1, crossHair: 2 } as const;
export type GridType = (typeof GridType)[keyof typeof GridType];

// How closely the grid divides the time or the level axis.
export const GridDensity = { coarse: 0, medium: 1, fine: 2 } as const;
export type GridDensity = (typeof GridDensity)[keyof typeof GridDensity];

// Beats' grid below the beat: halves and quarters, or thirds of the beat and their halves.
export const BeatDivision = { regular: 0, triplets: 1 } as const;
export type BeatDivision = (typeof BeatDivision)[keyof typeof BeatDivision];

// Every enumeration, by name, for code generation.
export const enums = {
  ChannelView,
  ViewLayout,
  TriggerMode,
  TriggerSlope,
  TimeBase,
  BeatSpan,
  Toggle,
  DisplayMode,
  Group,
  FrameMode,
  GridType,
  GridDensity,
  BeatDivision,
} as const;
export type EnumName = keyof typeof enums;
