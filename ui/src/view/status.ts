// Status chips of the top bar (DESIGN 8.5), derived from the latest frame and the trigger mode.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";

export type Tone = "neutral" | "ok" | "hold" | "warn";

export interface Chip {
  readonly text: string;
  readonly tone: Tone;
}

// Normal and Single show "Waiting" once no frame has arrived for this long.
export const waitingAfterSeconds = 0.25;

export function statusChips(
  frame: Frame | undefined,
  mode: string,
  secondsSinceFrame: number,
  panSeconds: number,
): Chip[] {
  const chips: Chip[] = [];
  if (frame?.frozen === true) {
    chips.push({ text: "Frozen", tone: "hold" });
    if (panSeconds > 0) {
      chips.push({ text: `−${panSeconds.toFixed(2)} s`, tone: "neutral" });
    }
    return chips;
  }
  if (frame?.synced === true) {
    chips.push({ text: "Sync", tone: "ok" });
    if (frame.limited) {
      chips.push({ text: "Span limited", tone: "warn" });
    }
    return chips;
  }
  if (frame?.roll === true) {
    return [{ text: "Roll", tone: "neutral" }];
  }
  const waiting =
    (mode === "normal" || mode === "single") &&
    (frame === undefined || secondsSinceFrame > waitingAfterSeconds);
  if (waiting) {
    chips.push({ text: "Waiting", tone: "warn" });
  } else if (frame?.triggered === true) {
    chips.push({ text: "Trig’d", tone: "ok" });
  }
  chips.push({ text: mode, tone: "neutral" });
  return chips;
}
