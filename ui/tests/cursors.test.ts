import { describe, expect, test } from "vitest";

import {
  cursorReadout,
  initialCursors,
  initialLevels,
  levelReadout,
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
    expect(noteName(432, 432)).toBe("A4");
    expect(noteName(440, 432)).toBe("A4 +32 ct");
    expect(noteName(25000)).toBeUndefined();
  });

  // One label and value per row, in the order A, B, Δt, f, Note, Δb, Δs; every row a user reads
  // stays when it has no value, a dash, so the readout keeps its shape as the cursors move.
  const rows = (list: readonly { label: string; value: string }[]) =>
    list.map(({ label, value }) => `${label} ${value}`);

  test("read out where A and B are, the distance, its frequency and note, and samples", () => {
    expect(
      rows(
        cursorReadout(measure({ a: 0.25, b: 0.75 }, 0.01), 48000, 440, {
          a: "+1.0 ms",
          b: "+6.0 ms",
        }),
      ),
    ).toEqual([
      "A +1.0 ms",
      "B +6.0 ms",
      "Δt 5 ms",
      "f ≈ 200.0 Hz",
      "Note G3 +35 ct",
      "Δs 240.0 samples",
    ]);
  });

  test("read out the distance in beats in a tempo-synced sweep only", () => {
    expect(rows(cursorReadout(measure({ a: 0, b: 0.25 }, 2, 4, 4)))).toContain(
      "Δb 1.00 beats",
    );
    expect(
      rows(cursorReadout(measure({ a: 0.25, b: 0.75 }, 0.009_090_9))).some(
        (row) => row.startsWith("Δb"),
      ),
    ).toBe(false);
  });

  test("keep f and Note, as dashes, when the cursors coincide or the note is out of range", () => {
    expect(rows(cursorReadout(measure({ a: 0.5, b: 0.5 }, 1, 4, 4)))).toEqual([
      "Δt 0 ms",
      "f —",
      "Note —",
      "Δb 0 beats",
    ]);
    // 2 Hz is below the audible range: no frequency, no note.
    expect(rows(cursorReadout(measure({ a: 0, b: 0.25 }, 2)))).toEqual([
      "Δt 500 ms",
      "f —",
      "Note —",
    ]);
  });
});

describe("level cursors", () => {
  test("start 6 and 12 dB below the top", () => {
    expect(initialLevels(1)).toEqual({ first: 0.5, second: 0.25 });
  });

  test("read out each level and the difference, on either side of zero", () => {
    const rows = (levels: { first: number; second: number }) =>
      levelReadout(levels).map(({ label, value }) => `${label} ${value}`);
    expect(rows({ first: 0.5, second: -0.25 })).toEqual([
      `1 ${minus}6.0 dBFS`,
      `2 ${minus}12.0 dBFS`,
      "Δ 6.0 dB",
    ]);
    expect(rows({ first: 0, second: 1 })).toEqual([
      `1 ${minus}∞ dBFS`,
      "2 0.0 dBFS",
      "Δ —",
    ]);
    expect(rows({ first: 1e-6, second: 1 })).toEqual([
      `1 ${minus}∞ dBFS`,
      "2 0.0 dBFS",
      "Δ —",
    ]);
  });
});
