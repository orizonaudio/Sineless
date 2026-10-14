import { describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  analysisGateDbfs,
  crossingHysteresis,
  periodTolerance,
  triggerPosition,
} from "../../protocol/src/limits.ts";
import {
  cycleAt,
  cycleBounds,
  hoverReadout,
  levelAt,
  readPosition,
  risingCrossings,
  traceAt,
} from "../src/view/hover.ts";
import { fullView, type ViewWindow } from "../src/view/zoom.ts";

const minus = "−";
const sampleRate = 48000;

// A raw frame of `values`, one sample per point, covering the whole span.
function rawFrame(values: Float32Array, extra: Partial<Frame> = {}): Frame {
  return {
    sequence: 0,
    mode: FrameMode.raw,
    triggered: true,
    frozen: false,
    roll: false,
    pointCount: values.length,
    samplesPerPoint: 1,
    startFraction: 0,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    partial: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    panReach: 0,
    invalidSamples: 0,
    invalidLanes: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 4,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: values.length,
    views: new Map(),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
    ...extra,
  };
}

// The hover context for one trace of `values` in `frame`, drawn over the whole screen.
function context(
  frame: Frame,
  values: Float32Array,
  extra: { roll?: boolean; view?: ViewWindow } = {},
) {
  return {
    frame,
    traces: [{ name: "Left", values }],
    overlaid: 0,
    layout: ViewLayout.overlaid,
    view: extra.view ?? fullView,
    sampleRate,
    a4: 440,
    roll: extra.roll ?? false,
  };
}

// Min/max columns of a 0.5-amplitude sine with a cycle every `perCycle` columns.
function sineColumns(columns: number, perCycle: number): Float32Array {
  const values = new Float32Array(columns * 2);
  for (let column = 0; column < columns; column++) {
    const value = 0.5 * Math.sin((2 * Math.PI * (column + 0.5)) / perCycle);
    values[column * 2] = column % perCycle === 0 ? -0.01 : value;
    values[column * 2 + 1] = column % perCycle === 0 ? 0.01 : value;
  }
  return values;
}

// `cycles` periods of a 0.5-amplitude sine over `length` samples.
function sine(length: number, cycles: number): Float32Array {
  return Float32Array.from(
    { length },
    (_, index) => 0.5 * Math.sin((2 * Math.PI * cycles * index) / length),
  );
}

describe("hover readout", () => {
  test("picks the stacked lane under the pointer, or the overlaid trace", () => {
    expect(traceAt(3, ViewLayout.stacked, 0.1, 0)).toBe(0);
    expect(traceAt(3, ViewLayout.stacked, 0.5, 0)).toBe(1);
    expect(traceAt(3, ViewLayout.stacked, 0.9, 0)).toBe(2);
    expect(traceAt(3, ViewLayout.overlaid, 0.9, 1)).toBe(1);
  });

  test("finds cycles around a DC offset, and none in noise or silence", () => {
    const offset = sine(480, 4).map((value) => value + 0.3);
    expect(cycleAt(rawFrame(offset), offset, 0.4)).toBeCloseTo(0.25, 3);
    let seed = 7;
    const noise = Float32Array.from({ length: 4800 }, () => {
      seed = (seed * 1103515245 + 12345) % 2147483648;
      return seed / 2147483648 - 0.5;
    });
    const noiseFrame = rawFrame(noise);
    const found = [0.2, 0.4, 0.6, 0.8].filter(
      (position) => cycleAt(noiseFrame, noise, position) !== undefined,
    );
    expect(found.length).toBeLessThanOrEqual(1);
    const silence = new Float32Array(480);
    expect(risingCrossings(rawFrame(silence), silence)).toEqual([]);
  });

  test.each([Number.NaN, Infinity, -Infinity])(
    "measures around samples that are %s, as the engine does",
    (broken) => {
      const values = sine(480, 4);
      const clean = risingCrossings(rawFrame(values), values);
      const spiked = values.slice();
      // Away from the crossings, which a broken sample would hide.
      for (const index of [30, 150, 270]) {
        spiked[index] = broken;
      }
      // The level is the mean of the finite samples, a hair from the clean one's.
      const found = risingCrossings(rawFrame(spiked), spiked);
      expect(found).toHaveLength(clean.length);
      found.forEach((position, index) => {
        expect(position).toBeCloseTo(clean[index] ?? 0, 3);
      });
      const columns = new Float32Array(spiked.length * 2);
      spiked.forEach((value, index) => {
        columns[index * 2] = value;
        columns[index * 2 + 1] = value;
      });
      const minMax = rawFrame(columns, {
        mode: FrameMode.minMax,
        pointCount: spiked.length,
      });
      expect(risingCrossings(minMax, columns)).toHaveLength(clean.length);
    },
  );

  test("finds rising zero crossings only", () => {
    const values = sine(480, 4); // 120-sample cycles.
    const crossings = risingCrossings(rawFrame(values), values);
    expect(crossings).toHaveLength(3); // The one at the very start is not crossed into.
    expect(crossings[0]).toBeCloseTo(120 / 480, 2);
  });

  test("finds crossings inside min/max columns and at their edges", () => {
    // Eight columns: below, below, spanning (rising), above, above, below, above (an edge
    // crossing), spanning (falling).
    const values = Float32Array.from([
      -0.5, -0.4, -0.3, -0.2, -0.1, 0.2, 0.3, 0.4, 0.3, 0.5, -0.4, -0.2, 0.2,
      0.3, -0.2, 0.1,
    ]);
    const frame = rawFrame(new Float32Array(80), {
      mode: FrameMode.minMax,
      pointCount: 8,
      samplesPerPoint: 10,
    });
    const crossings = risingCrossings(frame, values);
    expect(crossings).toHaveLength(2);
    expect(crossings[0]).toBeCloseTo(2.5 / 8, 6); // Inside the third column.
    expect(crossings[1]).toBeCloseTo(6 / 8, 6); // The edge before the seventh.
  });

  test("measures the cycle around the pointer and the level under it", () => {
    const values = sine(480, 4);
    const frame = rawFrame(values);
    expect(cycleAt(frame, values, 0.4)).toBeCloseTo(0.25, 3);
    expect(cycleAt(frame, values, 0.1)).toBeUndefined(); // No rising crossing before it.
    expect(levelAt(frame, values, 30 / 480)).toBeCloseTo(0.5, 3); // A crest.
    const bounds = cycleBounds(frame, values, 0.4);
    expect(bounds?.start).toBeCloseTo(0.25, 3);
    expect(bounds?.end).toBeCloseTo(0.5, 3);
  });

  test("says which point or column it read", () => {
    const values = sine(480, 4);
    expect(readPosition(rawFrame(values), 150.4 / 480)).toBeCloseTo(
      150 / 480,
      6,
    );
    const columns = rawFrame(new Float32Array(480), {
      mode: FrameMode.minMax,
      pointCount: 96,
      samplesPerPoint: 5,
    });
    expect(readPosition(columns, 0.5)).toBeCloseTo(48.5 / 96, 6);
    expect(readPosition(columns, 1.5)).toBeUndefined();
  });

  test("reads out the position from the trigger, level, cycle and note", () => {
    // 480 samples (10 ms at 48 kHz) of 4 cycles: 400 Hz.
    const values = sine(480, 4);
    const readout = hoverReadout(
      {
        frame: rawFrame(values),
        traces: [{ name: "Left", values }],
        overlaid: 0,
        layout: ViewLayout.overlaid,
        view: fullView,
        sampleRate,
        a4: 440,
        roll: false,
      },
      150 / 480,
      0.5,
    );
    expect(readout?.title).toBe("Left");
    expect(readout?.lines).toEqual([
      "+2.13 ms",
      `${minus}6.0 dBFS`,
      "2.5 ms · 120.0 samples",
      "≈ 400.0 Hz · G4 +35 ct",
    ]);
    expect(readout?.x).toBeCloseTo(150 / 480, 6);
    expect(readout?.cycle?.start).toBeCloseTo(0.25, 3);
    expect(readout?.cycle?.end).toBeCloseTo(0.5, 3);
    expect(readout?.top).toBe(0);
    expect(readout?.bottom).toBe(1);
  });

  test("marks the stacked lane under the pointer only", () => {
    const values = sine(480, 4);
    const readout = hoverReadout(
      {
        frame: rawFrame(values),
        traces: [
          { name: "Kick", values },
          { name: "Bass", values },
          { name: "Hats", values },
        ],
        overlaid: 0,
        layout: ViewLayout.stacked,
        view: fullView,
        sampleRate,
        a4: 440,
        roll: false,
      },
      0.5,
      0.5,
    );
    expect(readout?.title).toBe("Bass");
    expect(readout?.top).toBeCloseTo(1 / 3, 6);
    expect(readout?.bottom).toBeCloseTo(2 / 3, 6);
  });

  test("marks the point and cycle as screen positions, clipped to the view", () => {
    const values = sine(480, 4);
    const context = {
      frame: rawFrame(values),
      traces: [{ name: "Left", values }],
      overlaid: 0,
      layout: ViewLayout.overlaid,
      view: { start: 0.3, end: 0.8 },
      sampleRate,
      a4: 440,
      roll: false,
    };
    // Screen 0.025 is span 0.3125, sample 150: the cycle from 0.25 to 0.5 starts off screen.
    const readout = hoverReadout(context, 0.025, 0.5);
    expect(readout?.x).toBeCloseTo(0.025, 6);
    expect(readout?.cycle?.start).toBe(0);
    expect(readout?.cycle?.end).toBeCloseTo(0.4, 6);
  });

  test("rounds a cycle read from min/max columns and names it without cents", () => {
    // 96 columns over 10 ms, one 0.5-amplitude cycle every 32 columns: 300 Hz.
    const columns = 96;
    const values = new Float32Array(columns * 2);
    for (let column = 0; column < columns; column++) {
      const value = 0.5 * Math.sin((2 * Math.PI * (column + 0.5)) / 32);
      values[column * 2] = column % 32 === 0 ? -0.01 : value;
      values[column * 2 + 1] = column % 32 === 0 ? 0.01 : value;
    }
    const frame = rawFrame(new Float32Array(480), {
      mode: FrameMode.minMax,
      pointCount: columns,
      samplesPerPoint: 5,
    });
    const readout = hoverReadout(
      {
        frame,
        traces: [{ name: "Left", values }],
        overlaid: 0,
        layout: ViewLayout.overlaid,
        view: fullView,
        sampleRate,
        a4: 440,
        roll: false,
      },
      0.5,
      0.5,
    );
    expect(readout?.lines.slice(2)).toEqual([
      "≈ 3.33 ms · 160 samples",
      "≈ 300.0 Hz · D4",
    ]);
  });

  test("names the musical position and project time in a synced sweep", () => {
    // One bar of 4/4 at 120 BPM (2 s) starting at bar 3, 4 s into the project.
    const values = new Float32Array(96000);
    const readout = hoverReadout(
      {
        frame: rawFrame(values, {
          synced: true,
          cycleStart: 8,
          cycleSeconds: 4,
          spanQuarterNotes: 4,
        }),
        traces: [{ name: "Kick", values }],
        overlaid: 0,
        layout: ViewLayout.overlaid,
        view: fullView,
        sampleRate,
        a4: 440,
        roll: false,
      },
      0.625,
      0.5,
    );
    expect(readout?.lines[0]).toBe("3.3.3 · 0:05.250");
  });

  test("reads a frame that covers part of the span as the part it covers", () => {
    // The frozen view zoomed in: 480 samples of 4 cycles cover the span from 0.25 to 0.75.
    const values = sine(480, 4);
    const frame = rawFrame(values, { viewStart: 0.25, viewEnd: 0.75 });
    const crest = 0.25 + (30 / 480) * 0.5; // Sample 30, a crest.
    expect(levelAt(frame, values, crest)).toBeCloseTo(0.5, 3);
    expect(readPosition(frame, crest)).toBeCloseTo(crest, 6);
    // Its cycles last as long as unzoomed: 120 samples, 2.5 ms.
    const readout = hoverReadout(context(frame, values), 0.5, 0.5);
    expect(readout?.lines[2]).toBe("2.5 ms · 120.0 samples");
    // Min/max columns zoomed in alike: a cycle of 32 of them is still fine enough to measure.
    const columns = sineColumns(96, 32);
    const zoomedColumns = rawFrame(new Float32Array(480), {
      mode: FrameMode.minMax,
      pointCount: 96,
      samplesPerPoint: 5,
      viewStart: 0.25,
      viewEnd: 0.75,
    });
    expect(cycleAt(zoomedColumns, columns, 0.5)).toBeCloseTo(
      (32 / 96) * 0.5,
      3,
    );
  });

  test("reads the first and last points and columns, and nothing off the frame", () => {
    const values = sine(480, 4);
    const raw = rawFrame(values);
    expect(levelAt(raw, values, 0)).toBe(Math.abs(values[0] ?? NaN));
    expect(levelAt(raw, values, 479 / 480)).toBeCloseTo(
      Math.abs(values[479] ?? NaN),
      6,
    );
    expect(levelAt(raw, values, -0.2)).toBeUndefined();
    expect(levelAt(raw, values, 1.2)).toBeUndefined();
    expect(readPosition(raw, 0)).toBe(0);
    expect(readPosition(raw, -0.2)).toBeUndefined();
    expect(readPosition(raw, 1.2)).toBeUndefined();
    // Just past the last point, nearer a point the frame does not have: no reading, rather
    // than a level of nothing.
    expect(levelAt(raw, values, 479.6 / 480)).toBeUndefined();
    expect(readPosition(raw, 479.6 / 480)).toBeUndefined();
    const columns = rawFrame(new Float32Array(480), {
      mode: FrameMode.minMax,
      pointCount: 96,
      samplesPerPoint: 5,
    });
    // Each column's level is its larger magnitude: the first column's low, the last's high.
    const peaks = new Float32Array(192).fill(0.1);
    peaks[0] = -0.9;
    peaks[191] = 0.8;
    expect(levelAt(columns, peaks, 0)).toBeCloseTo(0.9, 6);
    expect(levelAt(columns, peaks, 0.999)).toBeCloseTo(0.8, 6);
    expect(readPosition(columns, 0)).toBeCloseTo(0.5 / 96, 6);
    expect(readPosition(columns, 0.999)).toBeCloseTo(95.5 / 96, 6);
    expect(levelAt(columns, peaks, -0.01)).toBeUndefined();
    expect(readPosition(columns, -0.01)).toBeUndefined();
    expect(levelAt(columns, peaks, 1)).toBeUndefined();
    expect(readPosition(columns, 1)).toBeUndefined();
  });

  test("shows nothing before there is anything to read", () => {
    const values = sine(480, 4);
    const empty = rawFrame(new Float32Array(0));
    expect(levelAt(empty, values, 0.5)).toBeUndefined();
    expect(readPosition(empty, 0.5)).toBeUndefined();
    expect(
      hoverReadout(
        { ...context(rawFrame(values), values), traces: [] },
        0.5,
        0.5,
      ),
    ).toBeUndefined();
    expect(
      hoverReadout(
        { ...context(rawFrame(values), values), sampleRate: 0 },
        0.5,
        0.5,
      ),
    ).toBeUndefined();
  });

  test("names a trace even when the overlaid one is no longer drawn", () => {
    // A track removed while it was the one described: the last trace drawn is.
    expect(traceAt(2, ViewLayout.overlaid, 0.5, 5)).toBe(1);
    expect(traceAt(1, ViewLayout.stacked, 0.9, 3)).toBe(0);
  });

  test("measures cycles only above the analysis gate, as the engine does", () => {
    // RMS of a sine is its amplitude over √2.
    const gate = 10 ** (analysisGateDbfs / 20);
    const at = (rms: number) =>
      sine(480, 4).map((value) => (value / 0.5) * rms * Math.SQRT2);
    const above = at(gate * 2);
    const below = at(gate / 2);
    expect(cycleAt(rawFrame(above), above, 0.4)).toBeCloseTo(0.25, 3);
    expect(cycleAt(rawFrame(below), below, 0.4)).toBeUndefined();
  });

  test("counts a crossing once when noise inside the hysteresis rides on it", () => {
    // Amplitude 1: a range of 2. Ripple well inside the hysteresis band crosses the mean
    // several times around each rising crossing.
    const ripple = 0.4 * crossingHysteresis * 2;
    const values = Float32Array.from(
      { length: 480 },
      (_, index) =>
        Math.sin((2 * Math.PI * 4 * index) / 480) +
        ripple * Math.sin((2 * Math.PI * index) / 5),
    );
    const crossings = risingCrossings(rawFrame(values), values);
    expect(crossings).toHaveLength(3);
    expect(cycleAt(rawFrame(values), values, 0.4)).toBeCloseTo(0.25, 2);
  });

  test("measures a cycle beside one like it, and none where the signal does not repeat", () => {
    // Cycles of 120, 120, 120, 200 and 200 samples: rising crossings at 120, 240, 360 and 560.
    const periods = [120, 120, 120, 200, 200];
    const values = new Float32Array(periods.reduce((a, b) => a + b, 0));
    let index = 0;
    for (const period of periods) {
      for (let sample = 0; sample < period; sample++) {
        values[index++] = 0.5 * Math.sin((2 * Math.PI * sample) / period);
      }
    }
    const frame = rawFrame(values);
    const length = values.length;
    // From 240 to 360: like the cycle before it, though not the one after.
    expect(cycleAt(frame, values, 300 / length)).toBeCloseTo(120 / length, 2);
    // From 360 to 560: like neither the cycle before it nor any after it.
    expect(cycleAt(frame, values, 460 / length)).toBeUndefined();
  });

  test.each([
    [0.8, true],
    [1.25, false],
  ])(
    "counts cycles %s of the engine's tolerance apart as repeating: %s",
    (share, repeats) => {
      // Rising crossings at 120 and 240, then a cycle that differs from the one before by
      // `share` of the tolerance; the cycle measured has only that one beside it. Within a
      // fifth of the tolerance either way, whichever of the two lengths it is taken from.
      const longer = Math.round(120 / (1 - share * periodTolerance));
      const periods = [120, 120, longer, 120];
      const values = new Float32Array(periods.reduce((a, b) => a + b, 0));
      let index = 0;
      for (const period of periods) {
        for (let sample = 0; sample < period; sample++) {
          values[index++] = 0.5 * Math.sin((2 * Math.PI * sample) / period);
        }
      }
      const frame = rawFrame(values);
      const middle = (240 + longer / 2) / values.length;
      const cycle = cycleAt(frame, values, middle);
      if (repeats) {
        expect(cycle).toBeCloseTo(longer / values.length, 2);
      } else {
        expect(cycle).toBeUndefined();
      }
    },
  );

  test("leaves min/max columns too coarse for a cycle unmeasured", () => {
    // A cycle every 8 columns: each crossing is only known to within a column, an eighth of it.
    const values = sineColumns(96, 8);
    const frame = rawFrame(new Float32Array(480), {
      mode: FrameMode.minMax,
      pointCount: 96,
      samplesPerPoint: 5,
    });
    expect(cycleAt(frame, values, 0.5)).toBeUndefined();
    expect(hoverReadout(context(frame, values), 0.5, 0.5)?.lines).toHaveLength(
      2,
    );
  });

  test("shows no minus sign at the trigger point itself", () => {
    const values = sine(480, 4);
    const readout = hoverReadout(
      context(rawFrame(values), values),
      triggerPosition,
      0.5,
    );
    expect(readout?.lines[0]).not.toContain(minus);
  });

  test("counts time back from now in a rolling span", () => {
    // 480 samples at 48 kHz: the span is 10 ms, its end now.
    const values = sine(480, 4);
    const readout = hoverReadout(
      context(rawFrame(values), values, { roll: true }),
      0.75,
      0.5,
    );
    expect(readout?.lines[0]).toBe(`${minus}2.5 ms`);
  });
});
