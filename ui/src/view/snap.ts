// Snap targets for cursors, taken from the frame on screen: zero crossings and the positions of
// the trace's extremes for time cursors, the extremes' values and zero for level cursors. Values
// that are not finite (from a broken signal upstream) are passed over, as the engine measures
// around them.
import { FrameMode } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { columnPosition, pointPosition } from "../render/trace-geometry.ts";

// Span positions (0 to 1) where `values` (one view of `frame`) crosses zero: passes from one
// side of it to the other, whether drawn as raw points or min/max columns. Raw points cross
// interpolated between them; a min/max column that reaches both sides, at its centre; and
// between points or columns on either side of exact zeros (as at digital silence), in the middle
// of those zeros, or at the edge between two columns. Touching zero, or leaving or entering
// silence on one side, is no crossing, and none spans a value that is not finite.
export function zeroCrossings(frame: Frame, values: Float32Array): number[] {
  const crossings: number[] = [];
  if (frame.mode === FrameMode.minMax) {
    // The side of the last column off zero, and its index; 0 after one that reaches both.
    let side = 0;
    let sideColumn = 0;
    for (let column = 0; column < frame.pointCount; column++) {
      const low = values[column * 2] ?? 0;
      const high = values[column * 2 + 1] ?? 0;
      if (!Number.isFinite(low) || !Number.isFinite(high)) {
        side = 0; // Measured around: no crossing spans it.
        continue;
      }
      if (low < 0 && high > 0) {
        crossings.push(columnPosition(frame, column + 0.5));
        side = 0;
        continue;
      }
      const here = low < 0 ? -1 : high > 0 ? 1 : 0;
      if (here === 0) {
        continue; // Silence.
      }
      if (side !== 0 && here !== side) {
        crossings.push(columnPosition(frame, (sideColumn + 1 + column) / 2));
      }
      side = here;
      sideColumn = column;
    }
    return crossings;
  }
  // The last point off zero, and its index: a crossing is a change of side since it.
  let side = 0;
  let sideIndex = 0;
  for (let index = 0; index < frame.pointCount; index++) {
    const value = values[index] ?? 0;
    if (!Number.isFinite(value)) {
      side = 0; // Measured around: no crossing spans it.
      continue;
    }
    if (value === 0) {
      continue;
    }
    if (side !== 0 && Math.sign(value) !== Math.sign(side)) {
      const crossing =
        index === sideIndex + 1
          ? sideIndex + side / (side - value)
          : (sideIndex + index) / 2;
      crossings.push(pointPosition(frame, crossing));
    }
    side = value;
    sideIndex = index;
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
