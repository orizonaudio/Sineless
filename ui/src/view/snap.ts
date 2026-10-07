// Snap targets for cursors, taken from the frame on screen: zero crossings for time cursors,
// the trace's extremes and zero for level cursors.
import { FrameMode } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { columnPosition, pointPosition } from "../render/trace-geometry.ts";

// Span positions (0 to 1) where `values` (one view of `frame`) crosses zero. Raw points are
// interpolated; a min/max column that spans zero counts as a crossing at its centre.
export function zeroCrossings(frame: Frame, values: Float32Array): number[] {
  const crossings: number[] = [];
  if (frame.mode === FrameMode.minMax) {
    for (let column = 0; column < frame.pointCount; column++) {
      const low = values[column * 2] ?? 0;
      const high = values[column * 2 + 1] ?? 0;
      if (low < 0 && high >= 0) {
        crossings.push(columnPosition(frame, column + 0.5));
      }
    }
    return crossings;
  }
  for (let index = 0; index + 1 < frame.pointCount; index++) {
    const here = values[index] ?? 0;
    const next = values[index + 1] ?? 0;
    if ((here < 0 && next >= 0) || (here >= 0 && next < 0)) {
      crossings.push(pointPosition(frame, index + here / (here - next)));
    }
  }
  return crossings;
}

// Levels a level cursor snaps to: the highest and lowest value of `values`, and zero.
export function levelTargets(values: Float32Array): number[] {
  let highest = 0;
  let lowest = 0;
  for (const value of values) {
    highest = Math.max(highest, value);
    lowest = Math.min(lowest, value);
  }
  return [highest, lowest, 0];
}

// The target nearest to `value` if one is within `radius`, otherwise `value` itself.
export function snap(
  value: number,
  targets: readonly number[],
  radius: number,
): number {
  let best = value;
  let bestDistance = radius;
  for (const target of targets) {
    const distance = Math.abs(target - value);
    if (distance <= bestDistance) {
      best = target;
      bestDistance = distance;
    }
  }
  return best;
}
