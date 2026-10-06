import { describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import { amplitudeLabels, timeLabels } from "../src/view/axis-labels.ts";
import {
  formatDb,
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
  test("labels a single lane at full, half and zero", () => {
    const labels = amplitudeLabels(1, 2, ViewLayout.overlaid);
    expect(labels.map((label) => label.text)).toEqual([
      "+1.00",
      "+0.50",
      "0",
      `${minus}0.50`,
      `${minus}1.00`,
    ]);
    expect(labels.map((label) => label.position)).toEqual([
      0, 0.25, 0.5, 0.75, 1,
    ]);
  });

  test("keeps only the outer and zero labels on a short screen", () => {
    const labels = amplitudeLabels(1, 1, ViewLayout.overlaid, true);
    expect(labels.map((label) => label.text)).toEqual([
      "+1.00",
      "0",
      `${minus}1.00`,
    ]);
  });

  test("labels each stacked lane", () => {
    const labels = amplitudeLabels(0.5, 2, ViewLayout.stacked);
    expect(labels.map((label) => label.text)).toEqual([
      "+0.50",
      "0",
      "+0.50",
      "0",
    ]);
    expect(labels.map((label) => label.position)).toEqual([0, 0.25, 0.5, 0.75]);
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
