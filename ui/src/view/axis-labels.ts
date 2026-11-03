// Positions and texts of the screen's axis labels. Positions are fractions of the
// screen: `x` from the left, `y` from the top.
import { lanes } from "../render/trace-geometry.ts";
import { ViewLayout } from "../../../protocol/src/enums.ts";
import {
  gridColumns,
  gridLevelStepDb,
  gridLevelsBelowTop,
  gridMinimumLineGap,
  triggerPosition,
} from "../../../protocol/src/limits.ts";
import { formatDb, formatDbValue, formatSeconds } from "./format.ts";
import { fullView, toFrame, type ViewWindow } from "./zoom.ts";

export { gridColumns, triggerPosition };

// Level lines (protocol/src/limits.ts: gridLevelStepDb, gridLevelsBelowTop). Closest spacing (CSS
// px) between drawn level lines (gridMinimumLineGap), and between level labels.
const levelStepDb = gridLevelStepDb;
const levelsBelowTop = gridLevelsBelowTop;
export const minimumLineGap = gridMinimumLineGap;
export const minimumLabelGap = 14;

export function levelFraction(step: number): number {
  return 10 ** ((-levelStepDb * step) / 20);
}

// The steps below the top (0 is the top itself) shown in a half-lane `halfLanePx` tall: every
// 3 dB, else every 6 dB, else the top only, else none, so that neighbours stay `minimumGap`
// apart.
export function levelSteps(halfLanePx: number, minimumGap: number): number[] {
  for (const stride of [1, 2]) {
    const steps: number[] = [];
    for (let step = 0; step <= levelsBelowTop; step += stride) {
      steps.push(step);
    }
    // Each line's distance to the next one down; the last one's to the zero line.
    const fits = steps.every((step) => {
      const below =
        step + stride <= levelsBelowTop ? levelFraction(step + stride) : 0;
      return (levelFraction(step) - below) * halfLanePx >= minimumGap;
    });
    if (fits) {
      return steps;
    }
  }
  return halfLanePx >= minimumGap ? [0] : [];
}

export interface AxisLabel {
  readonly text: string;
  readonly position: number;
  // A second, fainter line, such as the project time under a bar.beat label.
  readonly detail?: string;
}

// Level labels in dB at the level lines of each lane: the top carries the unit, the zero line
// reads minus infinity. Overlaid views share one lane. A single lane is labelled on both halves;
// stacked lanes only above their zero line, so that labels at a lane boundary never collide.
export function amplitudeLabels(
  topDb: number,
  laneCount: number,
  layout: ViewLayout,
  screenHeight: number,
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  const viewLanes =
    layout === ViewLayout.stacked ? lanes(laneCount, layout) : lanes(1, layout);
  for (const lane of viewLanes) {
    const steps = levelSteps(
      (lane.halfHeight * screenHeight) / 2,
      minimumLabelGap,
    );
    const at = (fraction: number) =>
      (1 - (lane.center + fraction * lane.halfHeight)) / 2;
    const text = (step: number) =>
      step === 0 ? formatDb(topDb) : formatDbValue(topDb - levelStepDb * step);
    for (const step of steps) {
      labels.push({ text: text(step), position: at(levelFraction(step)) });
    }
    labels.push({ text: "\u2212\u221e", position: at(0) });
    if (viewLanes.length === 1) {
      for (const step of [...steps].reverse()) {
        labels.push({ text: text(step), position: at(-levelFraction(step)) });
      }
    }
  }
  return labels;
}

// Time labels every second grid division: relative to the trigger point, or back from the right
// edge (now, live) while running free. Under view zoom they show the times at those screen positions.
export function timeLabels(
  spanSeconds: number,
  freeRunning: boolean,
  view: ViewWindow = fullView,
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  for (let division = 2; division < gridColumns; division += 2) {
    const x = division / gridColumns;
    const frame = toFrame(view, x);
    const seconds = freeRunning
      ? -(1 - frame) * spanSeconds
      : (frame - triggerPosition) * spanSeconds;
    const text = formatSeconds(Math.abs(seconds));
    labels.push({ text: seconds < 0 ? `\u2212${text}` : text, position: x });
  }
  return labels;
}
