// Snap targets for cursors, taken from the frame on screen: zero crossings and the positions of
// the trace's extremes for time cursors, the extremes' values and zero for level cursors. Values
// that are not finite (from a broken signal upstream) are passed over, as the engine measures
// around them.
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
      if (
        Number.isFinite(low) &&
        Number.isFinite(high) &&
        low < 0 &&
        high >= 0
      ) {
        crossings.push(columnPosition(frame, column + 0.5));
      }
    }
    return crossings;
  }
  for (let index = 0; index + 1 < frame.pointCount; index++) {
    const here = values[index] ?? 0;
    const next = values[index + 1] ?? 0;
    if (!Number.isFinite(here) || !Number.isFinite(next)) {
      continue;
    }
    if ((here < 0 && next >= 0) || (here >= 0 && next < 0)) {
      crossings.push(pointPosition(frame, index + here / (here - next)));
    }
  }
  return crossings;
}

// Span positions of the highest and of the lowest point of `values` (one view of `frame`): the
// first raw point or min/max column centre that reaches each. None if no value is finite.
export function extremePositions(frame: Frame, values: Float32Array): number[] {
  const minMax = frame.mode === FrameMode.minMax;
  let highest = -1;
  let lowest = -1;
  let top = -Infinity;
  let bottom = Infinity;
  for (let index = 0; index < frame.pointCount; index++) {
    const high = (minMax ? values[index * 2 + 1] : values[index]) ?? 0;
    const low = (minMax ? values[index * 2] : values[index]) ?? 0;
    if (Number.isFinite(high) && high > top) {
      highest = index;
      top = high;
    }
    if (Number.isFinite(low) && low < bottom) {
      lowest = index;
      bottom = low;
    }
  }
  if (highest < 0 || lowest < 0) {
    return [];
  }
  const position = (index: number) =>
    minMax ? columnPosition(frame, index + 0.5) : pointPosition(frame, index);
  return [position(highest), position(lowest)];
}

// Levels a level cursor snaps to: the highest and lowest value of `values`, and zero.
export function levelTargets(values: Float32Array): number[] {
  let highest = 0;
  let lowest = 0;
  for (const value of values) {
    if (!Number.isFinite(value)) {
      continue;
    }
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
