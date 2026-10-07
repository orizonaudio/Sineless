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

// Tempo-synced spans: fractions of a beat (a quarter note), then bars.
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

// Length of each beat span: an amount of beats (quarter notes), or of bars, which follow the
// host time signature. Indexed by BeatSpan value.
export const beatSpanLengths: readonly {
  readonly amount: number;
  readonly inBars: boolean;
}[] = [
  { amount: 1 / 16, inBars: false },
  { amount: 1 / 8, inBars: false },
  { amount: 1 / 4, inBars: false },
  { amount: 1 / 2, inBars: false },
  { amount: 1, inBars: false },
  { amount: 2, inBars: false },
  { amount: 1, inBars: true },
  { amount: 2, inBars: true },
  { amount: 4, inBars: true },
];

export function beatSpanQuarterNotes(
  span: BeatSpan,
  barQuarterNotes: number,
): number {
  const length = beatSpanLengths[span] ?? { amount: 1, inBars: true };
  return length.inBars ? length.amount * barQuarterNotes : length.amount;
}

export const Toggle = { off: 0, on: 1 } as const;
export type Toggle = (typeof Toggle)[keyof typeof Toggle];

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
  Toggle,
  FrameMode,
} as const;
export type EnumName = keyof typeof enums;
