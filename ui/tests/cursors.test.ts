import { describe, expect, test } from "vitest";

import {
  cursorReadout,
  initialCursors,
  measure,
  noteName,
} from "../src/view/cursors.ts";

const minus = "−";

describe("time cursors", () => {
  test("start a third and two thirds across the visible part", () => {
    expect(initialCursors({ start: 0, end: 1 })).toEqual({
      a: 1 / 3,
      b: 2 / 3,
    });
    const zoomed = initialCursors({ start: 0.5, end: 0.8 });
    expect(zoomed.a).toBeCloseTo(0.6);
    expect(zoomed.b).toBeCloseTo(0.7);
  });

  test("measure the time between them in either order", () => {
    const measurement = measure({ a: 0.75, b: 0.25 }, 0.02);
    expect(measurement.seconds).toBeCloseTo(0.01);
    expect(measurement.hertz).toBeCloseTo(100);
    expect(measurement.quarterNotes).toBeUndefined();
  });

  test("have no frequency when they coincide", () => {
    expect(measure({ a: 0.4, b: 0.4 }, 1).hertz).toBeUndefined();
  });

  test("measure beats in a tempo-synced sweep", () => {
    // One bar of 4/4 at 120 BPM: 2 s, 4 beats.
    const measurement = measure({ a: 0, b: 0.25 }, 2, 4);
    expect(measurement.seconds).toBeCloseTo(0.5);
    expect(measurement.quarterNotes).toBeCloseTo(1);
  });

  test("name the nearest note with its offset in cents", () => {
    expect(noteName(440)).toBe("A4");
    expect(noteName(220.3)).toBe("A3 +2 ct");
    expect(noteName(261.63)).toBe("C4");
    expect(noteName(55 * 2 ** (-30 / 1200))).toBe(`A1 ${minus}30 ct`);
    expect(noteName(10)).toBeUndefined();
    expect(noteName(25000)).toBeUndefined();
  });

  test("read out time, frequency with note, and beats", () => {
    expect(cursorReadout(measure({ a: 0.25, b: 0.75 }, 0.009_090_9))).toEqual([
      "Δt 4.55 ms",
      "≈ 220.0 Hz · A3",
    ]);
    expect(cursorReadout(measure({ a: 0, b: 0.25 }, 2, 4))).toEqual([
      "Δt 500 ms",
      "≈ 2.0 Hz",
      "1 beat",
    ]);
    expect(cursorReadout(measure({ a: 0.5, b: 0.5 }, 1, 4))).toEqual([
      "Δt 0 ms",
      "0 beats",
    ]);
  });
});
