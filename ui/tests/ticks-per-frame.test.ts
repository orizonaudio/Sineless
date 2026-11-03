import { describe, expect, test } from "vitest";

import { fullRateUpToHz, lineHysteresis } from "../../protocol/src/limits.ts";
import { ticksPerFrame } from "../src/render/render-loop.ts";
import { randomFrom } from "./display-ticks.ts";

describe("ticks per frame", () => {
  // Every tick up to 90 Hz, and at most 90 frames a second on the common faster displays, each
  // frame held for the same whole number of ticks.
  test.each([
    [50, 1],
    [59.94, 1],
    [60, 1],
    [75, 1],
    [85, 1],
    [90, 1],
    [100, 2],
    [120, 2],
    [144, 2],
    [165, 2],
    [170, 2],
    [180, 2],
    [240, 3],
    [360, 4],
  ])("holds each frame for the right ticks at %f Hz", (rate, n) => {
    expect(ticksPerFrame(rate)).toBe(n);
  });

  // Each line is a whole multiple of the full rate. A first measure falls on its side; a later
  // one changes n only once it is past the line by the hysteresis, so a rate measured near a line
  // does not flip n.
  const lines = [1, 2, 3, 4].map((below) => ({
    below,
    line: fullRateUpToHz * below,
  }));
  test.each(lines)(
    "places a first measure on its side of the line at $line Hz",
    ({ below, line }) => {
      expect(ticksPerFrame(line * 0.999)).toBe(below);
      expect(ticksPerFrame(line * 1.001)).toBe(below + 1);
    },
  );

  test.each(lines)(
    "keeps n within the hysteresis either side of the line at $line Hz",
    ({ below, line }) => {
      for (const share of [-0.99, -0.5, 0, 0.5, 0.99]) {
        const rate = line * (1 + share * lineHysteresis);
        expect(ticksPerFrame(rate, below)).toBe(below);
        expect(ticksPerFrame(rate, below + 1)).toBe(below + 1);
      }
      expect(ticksPerFrame(line * (1 + 1.01 * lineHysteresis), below)).toBe(
        below + 1,
      );
      expect(ticksPerFrame(line * (1 - 1.01 * lineHysteresis), below + 1)).toBe(
        below,
      );
    },
  );

  test("keeps n while a measure wanders inside the hysteresis around the line", () => {
    const random = randomFrom(11);
    let n = ticksPerFrame(fullRateUpToHz);
    const first = n;
    for (let index = 0; index < 10000; index++) {
      const share = 0.75 * lineHysteresis * (2 * random() - 1);
      n = ticksPerFrame(fullRateUpToHz * (1 + share), n);
      expect(n).toBe(first);
    }
  });

  test("moves by several frames' worth at once", () => {
    expect(ticksPerFrame(240, 1)).toBe(3);
    expect(ticksPerFrame(60, 3)).toBe(1);
  });
});
