import { describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  cycleAt,
  hoverReadout,
  levelAt,
  risingCrossings,
  traceAt,
} from "../src/view/hover.ts";
import { fullView } from "../src/view/zoom.ts";

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
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
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

  test("finds rising zero crossings only", () => {
    const values = sine(480, 4); // 120-sample cycles.
    const crossings = risingCrossings(rawFrame(values), values);
    expect(crossings).toHaveLength(3); // The one at the very start is not crossed into.
    expect(crossings[0]).toBeCloseTo(120 / 480, 2);
  });

  test("measures the cycle around the pointer and the level under it", () => {
    const values = sine(480, 4);
    const frame = rawFrame(values);
    expect(cycleAt(frame, values, 0.4)).toBeCloseTo(0.25, 3);
    expect(cycleAt(frame, values, 0.1)).toBeUndefined(); // No rising crossing before it.
    expect(levelAt(frame, values, 30 / 480)).toBeCloseTo(0.5, 3); // A crest.
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
});
