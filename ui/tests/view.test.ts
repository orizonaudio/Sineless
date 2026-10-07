import { describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  amplitudeLabels,
  levelFraction,
  levelSteps,
  timeLabels,
} from "../src/view/axis-labels.ts";
import {
  formatDb,
  formatDbValue,
  formatDbfs,
  formatFrequency,
  formatSeconds,
  parseQuantity,
  signed,
} from "../src/view/format.ts";
import { statusChips } from "../src/view/status.ts";
import { spanSteps, stepLinear, stepThrough } from "../src/view/steps.ts";

const minus = "−";

describe("format", () => {
  test("formats times in ms below a second", () => {
    expect(formatSeconds(0.0005)).toBe("0.5 ms");
    expect(formatSeconds(0.02)).toBe("20 ms");
    expect(formatSeconds(0.25)).toBe("250 ms");
    expect(formatSeconds(1.5)).toBe("1.5 s");
    expect(formatSeconds(4)).toBe("4 s");
  });

  test("formats dBFS readouts", () => {
    expect(formatDbfs(1)).toBe("0.0");
    expect(formatDbfs(0.5)).toBe(`${minus}6.0`);
    expect(formatDbfs(0)).toBe(`${minus}∞`);
    expect(formatDb(0)).toBe("0 dB");
    expect(formatDb(-12)).toBe(`${minus}12 dB`);
    expect(formatDb(6)).toBe("+6 dB");
    expect(formatDbValue(-10.5)).toBe(`${minus}10.5`);
    expect(formatDbValue(-7.3 - 3)).toBe(`${minus}10.3`);
    expect(formatDbValue(0)).toBe("0");
  });

  test("marks frequency as approximate or unavailable", () => {
    expect(formatFrequency(220)).toBe("≈ 220.0");
    expect(formatFrequency(4400.4)).toBe("≈ 4400");
    expect(formatFrequency(undefined)).toBe("—");
  });

  test("signs values with a true minus", () => {
    expect(signed(-0.25, 2)).toBe(`${minus}0.25`);
    expect(signed(0.5, 2)).toBe("+0.50");
    expect(signed(-0.0001, 2)).toBe("0.00");
  });

  test("parses typed quantities", () => {
    expect(parseQuantity("20 ms")).toBeCloseTo(0.02);
    expect(parseQuantity("0.5s")).toBe(0.5);
    expect(parseQuantity("500 us")).toBeCloseTo(0.0005);
    expect(parseQuantity("-12 dB")).toBe(-12);
    expect(parseQuantity(`${minus}0.25`)).toBe(-0.25);
    expect(parseQuantity("20", 0.001)).toBeCloseTo(0.02);
    expect(parseQuantity("0,5")).toBe(0.5);
    expect(parseQuantity("fast")).toBeUndefined();
  });
});

describe("steps", () => {
  test("follows the 1-2-5 span sequence from 0.5 ms to 4 s", () => {
    expect(spanSteps[0]).toBe(0.0005);
    expect(spanSteps.at(-1)).toBe(4);
    expect(stepThrough(spanSteps, 0.02, 1)).toBe(0.05);
    expect(stepThrough(spanSteps, 0.02, -2)).toBe(0.005);
    expect(stepThrough(spanSteps, 0.023, 1)).toBe(0.05); // From the nearest step.
    expect(stepThrough(spanSteps, 4, 3)).toBe(4);
  });

  test("steps linearly within the range", () => {
    expect(stepLinear(0, 3, 0.01, -1, 1)).toBeCloseTo(0.03);
    expect(stepLinear(0.99, 5, 0.01, -1, 1)).toBe(1);
  });
});

describe("axis labels", () => {
  test("steps level lines by 3 dB, then 6 dB, then the top only as lanes shrink", () => {
    expect(levelSteps(200, 8)).toEqual([0, 1, 2, 3, 4]);
    // The 3 dB lines nearest the zero line are 0.103 of the half-lane apart.
    expect(levelSteps(70, 8)).toEqual([0, 2, 4]);
    expect(levelSteps(20, 8)).toEqual([0]);
    expect(levelSteps(5, 8)).toEqual([]);
  });

  test("places level lines where the linear trace reaches them", () => {
    expect(levelFraction(0)).toBe(1);
    expect(levelFraction(2)).toBeCloseTo(0.501, 3); // -6 dB
    expect(levelFraction(4)).toBeCloseTo(0.251, 3); // -12 dB
  });

  test("labels a single lane in dB on both halves", () => {
    const labels = amplitudeLabels(0, 2, ViewLayout.overlaid, 600);
    expect(labels.map((label) => label.text)).toEqual([
      "0 dB",
      `${minus}3`,
      `${minus}6`,
      `${minus}9`,
      `${minus}12`,
      `${minus}∞`,
      `${minus}12`,
      `${minus}9`,
      `${minus}6`,
      `${minus}3`,
      "0 dB",
    ]);
    const positions = labels.map((label) => label.position);
    expect(positions[0]).toBe(0);
    expect(positions[2]).toBeCloseTo((1 - 0.501) / 2, 3);
    expect(positions[5]).toBe(0.5);
    expect(positions[10]).toBe(1);
  });

  test("counts down from the top in 3 dB steps", () => {
    const texts = (topDb: number) =>
      amplitudeLabels(topDb, 1, ViewLayout.overlaid, 600)
        .slice(0, 5)
        .map((label) => label.text);
    expect(texts(3)).toEqual([
      "+3 dB",
      "0",
      `${minus}3`,
      `${minus}6`,
      `${minus}9`,
    ]);
    expect(texts(-1)).toEqual([
      `${minus}1 dB`,
      `${minus}4`,
      `${minus}7`,
      `${minus}10`,
      `${minus}13`,
    ]);
    expect(texts(-7.5)[1]).toBe(`${minus}10.5`);
  });

  test("labels every 6 dB on a short screen", () => {
    const labels = amplitudeLabels(0, 1, ViewLayout.overlaid, 200);
    expect(labels.map((label) => label.text)).toEqual([
      "0 dB",
      `${minus}6`,
      `${minus}12`,
      `${minus}∞`,
      `${minus}12`,
      `${minus}6`,
      "0 dB",
    ]);
  });

  test("labels stacked lanes above their zero line only", () => {
    const labels = amplitudeLabels(0, 2, ViewLayout.stacked, 400);
    expect(labels.map((label) => label.text)).toEqual([
      "0 dB",
      `${minus}6`,
      `${minus}12`,
      `${minus}∞`,
      "0 dB",
      `${minus}6`,
      `${minus}12`,
      `${minus}∞`,
    ]);
    expect(labels[3]?.position).toBe(0.25);
    expect(labels[4]?.position).toBe(0.5);
  });

  test("counts time from the trigger point, or back from now when rolling", () => {
    expect(timeLabels(0.02, false).map((label) => label.text)).toEqual([
      "2 ms",
      "6 ms",
      "10 ms",
      "14 ms",
    ]);
    expect(timeLabels(2, true).map((label) => label.text)).toEqual([
      `${minus}1.6 s`,
      `${minus}1.2 s`,
      `${minus}800 ms`,
      `${minus}400 ms`,
    ]);
  });
});

describe("status chips", () => {
  const frame = (overrides: Partial<Frame>): Frame => ({
    sequence: 0,
    mode: FrameMode.minMax,
    triggered: false,
    frozen: false,
    roll: false,
    pointCount: 0,
    samplesPerPoint: 1,
    startFraction: 0,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    cursor: undefined,
    cycleStart: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(),
    ...overrides,
  });

  test("shows triggered frames with the mode", () => {
    expect(statusChips(frame({ triggered: true }), "auto", 0, 0)).toEqual([
      { text: "Trig’d", tone: "ok" },
      { text: "auto", tone: "neutral" },
    ]);
  });

  test("shows waiting in Normal once frames stop", () => {
    expect(
      statusChips(frame({ triggered: true }), "normal", 1, 0)[0]?.text,
    ).toBe("Waiting");
    expect(statusChips(frame({ triggered: true }), "auto", 1, 0)[0]?.text).toBe(
      "Trig’d",
    );
  });

  test("shows sync, and a shortened span", () => {
    expect(statusChips(frame({ synced: true }), "auto", 0, 0)).toEqual([
      { text: "Sync", tone: "ok" },
    ]);
    expect(
      statusChips(frame({ synced: true, limited: true }), "auto", 0, 0)[1],
    ).toEqual({
      text: "Span limited",
      tone: "warn",
    });
  });

  test("shows roll and frozen with the pan offset", () => {
    expect(statusChips(frame({ roll: true }), "auto", 0, 0)).toEqual([
      { text: "Roll", tone: "neutral" },
    ]);
    expect(statusChips(frame({ frozen: true }), "auto", 5, 0.12)).toEqual([
      { text: "Frozen", tone: "hold" },
      { text: "−0.12 s", tone: "neutral" },
    ]);
  });
});
