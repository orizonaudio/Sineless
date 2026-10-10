import { expect, test } from "vitest";

import { WheelSteps } from "../src/components/wheel-steps.ts";

function stepsFor(events: [pixels: number, time: number][]): number {
  const wheel = new WheelSteps();
  return events.reduce(
    (total, [pixels, time]) => total + wheel.add(pixels, time),
    0,
  );
}

test("steps once per mouse wheel click, whatever distance it reports", () => {
  expect(stepsFor([[100, 0]])).toBe(-1); // Windows: a notch down.
  expect(stepsFor([[-4, 0]])).toBe(1); // WebKit: a notch up.
  expect(
    stepsFor([
      [-4, 0],
      [-4, 300],
      [-4, 600],
    ]),
  ).toBe(3);
});

test("steps by distance through a trackpad's stream of small events", () => {
  const swipe = Array.from({ length: 40 }, (_, index): [number, number] => [
    -3,
    index * 10,
  ]);
  expect(stepsFor(swipe)).toBe(2); // The first event, then 117 px.
});

test("steps once per notch of a wheel spun fast", () => {
  const spin = Array.from({ length: 5 }, (_, index): [number, number] => [
    100,
    index * 30,
  ]);
  expect(stepsFor(spin)).toBe(-5);
});

test("starts over when a stream turns around", () => {
  const wheel = new WheelSteps();
  expect(wheel.add(-3, 0)).toBe(1);
  expect(wheel.add(-90, 10)).toBe(0);
  expect(wheel.add(60, 20)).toBe(0); // Down now: the 90 px up are dropped.
  expect(wheel.add(60, 30)).toBe(-1);
});
