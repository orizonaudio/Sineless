// Step sequences for drag fields and the scroll wheel.

// 1-2-5 steps across the time span range, as on hardware scopes.
export const spanSteps = [
  0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 4,
] as const;

export const holdOffSteps = [
  0, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1,
] as const;

// Moves `delta` steps from the step nearest to `value` (nearest in log terms for positive steps).
export function stepThrough(
  steps: readonly number[],
  value: number,
  delta: number,
): number {
  let nearest = 0;
  let best = Infinity;
  steps.forEach((step, index) => {
    const distance =
      step > 0 && value > 0
        ? Math.abs(Math.log(step / value))
        : Math.abs(step - value);
    if (distance < best) {
      best = distance;
      nearest = index;
    }
  });
  const index = Math.min(steps.length - 1, Math.max(0, nearest + delta));
  return steps[index] ?? value;
}

// Fine steps between those of a sequence: about 6% each, to three significant digits, within
// the sequence's range. A 6% step always changes the third digit, so a step within the range
// always moves the value.
export function stepFine(
  steps: readonly number[],
  value: number,
  delta: number,
): number {
  const minimum = steps[0] ?? value;
  const maximum = steps.at(-1) ?? value;
  const round = (x: number) => Number(x.toPrecision(3));
  let next = round(Math.max(value, minimum) * 1.06 ** delta);
  if (value === 0 && delta > 0) {
    next = steps.find((step) => step > 0) ?? value; // From zero, to the first step.
  }
  return Math.min(maximum, Math.max(minimum, next));
}

// Linear stepping, clamped and rounded to the step size.
export function stepLinear(
  value: number,
  delta: number,
  step: number,
  minimum: number,
  maximum: number,
): number {
  const next = Math.round((value + delta * step) / step) * step;
  return Math.min(maximum, Math.max(minimum, Number(next.toFixed(6))));
}
