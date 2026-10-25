// Status chips of the status bar, derived from the latest frame and the trigger mode.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";

import { formatSeconds } from "./format.ts";

export type Tone = "neutral" | "ok" | "hold" | "warn";

export interface Chip {
  readonly text: string;
  readonly tone: Tone;
}

// The trigger modes as the Trigger field and the mode chip name them.
export const triggerModeNames: Readonly<Record<string, string>> = {
  auto: "Auto",
  normal: "Normal",
  free: "Free",
  single: "Single",
};

// Normal and Single show "Waiting" once no frame has arrived for this long.
export const waitingAfterSeconds = 0.25;

// Whether lane `index` of `frame` shows samples that are not finite (NaN or infinite).
export function laneHoldsInvalid(frame: Frame, index: number): boolean {
  return index < 32 && (frame.invalidLanes & (2 ** index)) !== 0;
}

export function statusChips(
  frame: Frame | undefined,
  mode: string,
  secondsSinceFrame: number,
  panSeconds: number,
): Chip[] {
  const chips = modeChips(frame, mode, secondsSinceFrame, panSeconds);
  // Samples that are not finite reached the measurements, which measure around them. A problem
  // in the audio comes first, so it is never folded away when the chips don't all fit.
  if (
    frame !== undefined &&
    (frame.invalidSamples > 0 || frame.invalidLanes !== 0)
  ) {
    chips.unshift({ text: "Invalid samples", tone: "warn" });
  }
  return chips;
}

// The span was shortened to what can be kept: a beat span stepped down, or a Seconds span longer
// than the capture.
const spanLimited: Chip = { text: "Span limited", tone: "warn" };
// Beats mode before the host has reported a tempo: nothing is placed until it does.
const noTempo: Chip = { text: "No tempo", tone: "warn" };

function modeChips(
  frame: Frame | undefined,
  mode: string,
  secondsSinceFrame: number,
  panSeconds: number,
): Chip[] {
  const chips: Chip[] = [];
  if (frame?.frozen === true) {
    chips.push({ text: "Frozen", tone: "hold" });
    if (panSeconds > 0) {
      chips.push({ text: `−${formatSeconds(panSeconds)}`, tone: "neutral" });
    }
    if (frame.noTempo) {
      chips.push(noTempo);
    }
    if (frame.limited) {
      chips.push(spanLimited);
    }
    if (frame.partial) {
      chips.push({ text: "Partial", tone: "neutral" });
    }
    return chips;
  }
  if (frame?.synced === true && frame.noTempo) {
    return [noTempo];
  }
  if (frame?.synced === true) {
    chips.push({ text: "Sync", tone: "ok" });
    if (frame.limited) {
      chips.push(spanLimited);
    }
    if (frame.partial) {
      chips.push({ text: "Filling", tone: "neutral" });
    }
    return chips;
  }
  if (frame?.roll === true) {
    return [
      { text: "Roll", tone: "neutral" },
      ...(frame.limited ? [spanLimited] : []),
    ];
  }
  const waiting =
    (mode === "normal" || mode === "single") &&
    (frame === undefined || secondsSinceFrame > waitingAfterSeconds);
  if (waiting) {
    chips.push({ text: "Waiting", tone: "warn" });
  } else if (frame?.triggered === true) {
    chips.push({ text: "Trig’d", tone: "ok" });
  }
  // Named as the Trigger field names it: "Auto", not the setting's value "auto".
  chips.push({ text: triggerModeNames[mode] ?? mode, tone: "neutral" });
  return chips;
}
