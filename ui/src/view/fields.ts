// The number fields: how each shows its setting, steps it, and reads what is typed into it. The
// controls are built from this table, and the tests walk it, so every field is tested.
import type { SettingName } from "../../../protocol/src/settings.ts";
import {
  formatDb,
  formatPitch,
  formatSeconds,
  signed,
  type Quantity,
} from "./format.ts";
import {
  holdOffSteps,
  spanSteps,
  stepFine,
  stepLinear,
  stepThrough,
} from "./steps.ts";

export interface NumberField {
  readonly host: string; // The element the field goes into.
  readonly label: string;
  readonly format: (value: number) => string;
  readonly step: (value: number, delta: number, fine: boolean) => number;
  readonly quantity: Quantity;
  readonly bareScale?: number; // What a bare typed number is in: 0.001 for milliseconds.
}

export const numberFields: Partial<Record<SettingName, NumberField>> = {
  timeSpan: {
    host: "span-field",
    label: "Time span",
    format: formatSeconds,
    step: (value, delta, fine) =>
      fine
        ? stepFine(spanSteps, value, delta)
        : stepThrough(spanSteps, value, delta),
    quantity: "time",
    bareScale: 0.001,
  },
  verticalScale: {
    host: "scale-field",
    label: "Vertical scale",
    format: formatDb,
    step: (value, delta, fine) =>
      stepLinear(value, delta, fine ? 1 : 3, -60, 6),
    quantity: "level",
  },
  triggerLevel: {
    host: "level-field",
    label: "Trigger level",
    format: (value) => signed(value, 3),
    step: (value, delta, fine) =>
      stepLinear(value, delta, fine ? 0.001 : 0.01, -1, 1),
    quantity: "number",
  },
  triggerHysteresis: {
    host: "hysteresis-field",
    label: "Trigger hysteresis",
    format: (value) => value.toFixed(3),
    step: (value, delta, fine) =>
      stepLinear(value, delta, fine ? 0.001 : 0.005, 0, 0.5),
    quantity: "number",
  },
  triggerHoldOff: {
    host: "hold-off-field",
    label: "Trigger hold-off",
    format: formatSeconds,
    step: (value, delta, fine) =>
      fine
        ? stepFine(holdOffSteps, value, delta)
        : stepThrough(holdOffSteps, value, delta),
    quantity: "time",
    bareScale: 0.001,
  },
  concertPitch: {
    host: "concert-pitch-field",
    label: "Concert pitch",
    format: (value) => `${formatPitch(value)} Hz`,
    step: (value, delta, fine) => value + delta * (fine ? 0.1 : 1),
    quantity: "pitch",
  },
};
