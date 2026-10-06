// Positions and texts of the screen's axis labels (DESIGN 8.5). Positions are fractions of the
// screen: `x` from the left, `y` from the top.
import { lanes } from "../render/trace-geometry.ts";
import { ViewLayout } from "../../../protocol/src/enums.ts";
import { formatSeconds, signed } from "./format.ts";
import { fullView, toFrame, type ViewWindow } from "./zoom.ts";

export const gridColumns = 10;
export const gridRows = 8;
export const triggerPosition = 0.1;

export interface AxisLabel {
  readonly text: string;
  readonly position: number;
}

function amplitudeText(value: number, fullScale: number): string {
  if (value === 0) {
    return "0";
  }
  const decimals = fullScale >= 0.1 ? 2 : fullScale >= 0.01 ? 3 : 4;
  return signed(value, decimals);
}

// Amplitude labels at the top, middle and bottom of each lane; overlaid views share one lane.
// `compact` (a short screen) keeps only the top, zero and bottom labels.
export function amplitudeLabels(
  fullScale: number,
  laneCount: number,
  layout: ViewLayout,
  compact = false,
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  const viewLanes =
    layout === ViewLayout.stacked ? lanes(laneCount, layout) : lanes(1, layout);
  const fractions =
    viewLanes.length > 2
      ? [0]
      : viewLanes.length > 1
        ? [1, 0]
        : compact
          ? [1, 0, -1]
          : [1, 0.5, 0, -0.5, -1];
  for (const lane of viewLanes) {
    for (const fraction of fractions) {
      const y = (1 - (lane.center + fraction * lane.halfHeight)) / 2;
      labels.push({
        text: amplitudeText(fraction * fullScale, fullScale),
        position: y,
      });
    }
  }
  return labels;
}

// Time labels every second grid division: relative to the trigger point, or back from now
// while rolling. Under view zoom (FR-10) they show the times at those screen positions.
export function timeLabels(
  spanSeconds: number,
  roll: boolean,
  view: ViewWindow = fullView,
): AxisLabel[] {
  const labels: AxisLabel[] = [];
  for (let division = 2; division < gridColumns; division += 2) {
    const x = division / gridColumns;
    const frame = toFrame(view, x);
    const seconds = roll
      ? -(1 - frame) * spanSeconds
      : (frame - triggerPosition) * spanSeconds;
    const text = formatSeconds(Math.abs(seconds));
    labels.push({ text: seconds < 0 ? `\u2212${text}` : text, position: x });
  }
  return labels;
}
