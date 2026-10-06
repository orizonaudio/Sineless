import { expect, test } from "vitest";

import { FrameRateMeter } from "../src/spike/frame-rate";

test("reports 60 fps for frames every 1/60 s", () => {
  const meter = new FrameRateMeter();
  let rate: number | undefined;
  for (let frame = 0; frame <= 60; frame++) {
    rate = meter.tick((frame * 1000) / 60) ?? rate;
  }
  expect(rate).toBeCloseTo(60, 5);
});

test("reports nothing before a second has elapsed", () => {
  const meter = new FrameRateMeter();
  expect(meter.tick(0)).toBeUndefined();
  expect(meter.tick(500)).toBeUndefined();
});

test("reports a throttled rate", () => {
  const meter = new FrameRateMeter();
  meter.tick(0);
  expect(meter.tick(1000)).toBeCloseTo(1, 5);
});
