// The label of each value of the settings chosen in a toolbar field's popover, read by its
// segmented control and by the field's button, so the two never differ. Keyed by the protocol's
// value names: a value added there is a type error here until it has a label.
import type {
  BeatDivision,
  DisplayMode,
  GridDensity,
  GridType,
  TimeBase,
  TriggerMode,
  ViewLayout,
} from "../../../protocol/src/enums.ts";

type Labels<E> = Readonly<Record<keyof E, string>>;

export const choiceLabels: {
  readonly displayMode: Labels<typeof DisplayMode>;
  readonly timeBase: Labels<typeof TimeBase>;
  readonly viewLayout: Labels<typeof ViewLayout>;
  readonly triggerMode: Labels<typeof TriggerMode>;
  readonly gridType: Labels<typeof GridType>;
  readonly gridDensity: Labels<typeof GridDensity>;
  readonly beatDivision: Labels<typeof BeatDivision>;
} = {
  displayMode: { single: "Single", multi: "Multi" },
  timeBase: { beats: "Beats", free: "Free", trigger: "Trig" },
  viewLayout: { overlaid: "Overlay", stacked: "Stack" },
  triggerMode: { auto: "Auto", normal: "Normal", single: "Single" },
  gridType: { none: "None", grid: "Grid" },
  gridDensity: { coarse: "Coarse", medium: "Medium", fine: "Fine" },
  beatDivision: { regular: "Regular", triplets: "Triplets" },
};
