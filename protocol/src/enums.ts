// Enumerations shared by the engine and the UI. Values are stable: they are saved in projects.

export const ChannelView = { left: 0, right: 1, mid: 2, side: 3 } as const;
export type ChannelView = (typeof ChannelView)[keyof typeof ChannelView];

export const ViewLayout = { overlaid: 0, stacked: 1 } as const;
export type ViewLayout = (typeof ViewLayout)[keyof typeof ViewLayout];

export const TriggerMode = { auto: 0, normal: 1, free: 2, single: 3 } as const;
export type TriggerMode = (typeof TriggerMode)[keyof typeof TriggerMode];

export const TriggerSlope = { rising: 0, falling: 1 } as const;
export type TriggerSlope = (typeof TriggerSlope)[keyof typeof TriggerSlope];

export const TimeBase = { seconds: 0, beats: 1 } as const;
export type TimeBase = (typeof TimeBase)[keyof typeof TimeBase];

// Tempo-synced spans (FR-9): fractions of a beat (a quarter note), then bars.
export const BeatSpan = {
  sixteenthBeat: 0,
  eighthBeat: 1,
  quarterBeat: 2,
  halfBeat: 3,
  oneBeat: 4,
  twoBeats: 5,
  oneBar: 6,
  twoBars: 7,
  fourBars: 8,
} as const;
export type BeatSpan = (typeof BeatSpan)[keyof typeof BeatSpan];

export const FrameMode = { minMax: 0, raw: 1 } as const;
export type FrameMode = (typeof FrameMode)[keyof typeof FrameMode];

// Every enumeration, by name, for code generation.
export const enums = {
  ChannelView,
  ViewLayout,
  TriggerMode,
  TriggerSlope,
  TimeBase,
  BeatSpan,
  FrameMode,
} as const;
export type EnumName = keyof typeof enums;
