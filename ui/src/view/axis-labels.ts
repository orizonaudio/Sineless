// Positions and texts of the screen's axis labels. Positions are fractions of the
// screen: `x` from the left, `y` from the top.
import { lanes } from "../render/trace-geometry.ts";
import { ViewLayout } from "../../../protocol/src/enums.ts";
import {
  gridCoarseLevelStride,
  gridCoarseLineGap,
  gridFineLevelSubdivisions,
  gridFineLineGap,
  gridLevelStepDb,
  gridLevelsBelowTop,
  gridMediumLineGap,
  timeLabelColumns,
} from "../../../protocol/src/limits.ts";
import { formatDb, formatDbValue, formatSeconds } from "./format.ts";
import type { GridDensityName } from "./grid-settings.ts";
import { toFrame, type ViewWindow } from "./zoom.ts";

// Level lines (protocol/src/limits.ts: gridLevelStepDb, gridLevelsBelowTop). Closest spacing (CSS
// px) between drawn level lines at Medium's level step (gridMediumLineGap), and between level
// labels.
const levelStepDb = gridLevelStepDb;
const levelsBelowTop = gridLevelsBelowTop;
export const minimumLineGap = gridMediumLineGap;
export const minimumLabelGap = 14;

// Where a line `step` level steps below the top sits, as a fraction of the half-lane.
export function levelFraction(step: number): number {
  return dbFraction(levelStepDb * step);
}

// Where a line `db` dB below the top sits, as a fraction of the half-lane.
export function dbFraction(db: number): number {
  return 10 ** (-db / 20);
}

// The steps below the top (0 is the top itself) shown in a half-lane `halfLanePx` tall: every
// 3 dB, else every 6 dB, else the top only, else none, so that neighbours stay `minimumGap`
// apart.
export function levelSteps(halfLanePx: number, minimumGap: number): number[] {
  return strideSteps(halfLanePx, minimumGap, [1, gridCoarseLevelStride]);
}

// The steps of the first stride whose lines all stay `minimumGap` apart, else the top only, else
// none.
function strideSteps(
  halfLanePx: number,
  minimumGap: number,
  strides: readonly number[],
): number[] {
  for (const stride of strides) {
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

// A level line, `step` dB below the top; strong lines take the axis colour.
export interface LevelLine {
  readonly step: number;
  readonly strong: boolean;
}

// The level lines of a half-lane `halfLanePx` tall at a Density's level step, top first:
// - Medium: every 3 dB as levelSteps fits them;
// - Coarse: every 6 dB, else the top only, else none;
// - Fine: Medium's lines, strong, and between each two neighbouring 3 dB lines its 1 dB lines
//   where all three gaps they make stay gridFineLineGap apart (nearest zero, the narrowest, drop
//   first); with Medium thinned past 3 dB, Medium's lines alone.
export function levelLines(
  halfLanePx: number,
  density: GridDensityName,
): LevelLine[] {
  if (density === "coarse") {
    return strideSteps(halfLanePx, gridCoarseLineGap, [
      gridCoarseLevelStride,
    ]).map((step) => ({ step: step * levelStepDb, strong: false }));
  }
  const steps = levelSteps(halfLanePx, gridMediumLineGap);
  if (density === "medium") {
    return steps.map((step) => ({ step: step * levelStepDb, strong: false }));
  }
  const lines: LevelLine[] = [];
  const minor = levelStepDb / gridFineLevelSubdivisions;
  const everyStep = steps.length === levelsBelowTop + 1;
  for (const step of steps) {
    const top = step * levelStepDb;
    lines.push({ step: top, strong: true });
    if (!everyStep || step >= levelsBelowTop) {
      continue;
    }
    const inside = Array.from(
      { length: gridFineLevelSubdivisions - 1 },
      (_, index) => top + (index + 1) * minor,
    );
    const edges = [top, ...inside, top + levelStepDb];
    const fits = edges
      .slice(1)
      .every(
        (db, index) =>
          (dbFraction(edges[index] ?? 0) - dbFraction(db)) * halfLanePx >=
          gridFineLineGap,
      );
    if (fits) {
      for (const db of inside) {
        lines.push({ step: db, strong: false });
      }
    }
  }
  return lines;
}

// The steps (of gridLevelStepDb) level labels sit on at a Density's level step, at least
// minimumLabelGap apart: Coarse's on its 6 dB lines, Medium's and Fine's on the 3 dB ones.
export function levelLabelSteps(
  halfLanePx: number,
  density: GridDensityName,
): number[] {
  return density === "coarse"
    ? strideSteps(halfLanePx, minimumLabelGap, [gridCoarseLevelStride])
    : levelSteps(halfLanePx, minimumLabelGap);
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
  density: GridDensityName = "medium",
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  const viewLanes =
    layout === ViewLayout.stacked ? lanes(laneCount, layout) : lanes(1, layout);
  for (const lane of viewLanes) {
    const steps = levelLabelSteps(
      (lane.halfHeight * screenHeight) / 2,
      density,
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

// Time labels on the fifths of the screen, which every Density's divisions include: relative to
// the trigger point, `zero` across the span (the frame's triggerPosition), or back from the right
// edge (now, live) while running free. Under view zoom they show the times at those screen
// positions.
export function timeLabels(
  spanSeconds: number,
  freeRunning: boolean,
  view: ViewWindow,
  zero: number,
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  for (let column = 1; column < timeLabelColumns; column++) {
    const x = column / timeLabelColumns;
    const frame = toFrame(view, x);
    const seconds = freeRunning
      ? -(1 - frame) * spanSeconds
      : (frame - zero) * spanSeconds;
    const text = formatSeconds(Math.abs(seconds));
    labels.push({ text: seconds < 0 ? `\u2212${text}` : text, position: x });
  }
  return labels;
}
