import { describe, expect, test } from "vitest";

import {
  FrameMode,
  TriggerMode,
  ViewLayout,
} from "../../protocol/src/enums.ts";
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
  formatGain,
  formatOffset,
  formatPitch,
  formatFrequency,
  formatClock,
  formatSamples,
  formatSeconds,
  parseQuantity,
  signed,
} from "../src/view/format.ts";
import {
  laneHoldsInvalid,
  statusChips,
  waitingAfterSeconds,
} from "../src/view/status.ts";
import {
  holdOffSteps,
  spanSteps,
  stepFine,
  stepLinear,
  stepThrough,
} from "../src/view/steps.ts";

const minus = "−";

describe("format", () => {
  test("formats times in ms below a second", () => {
    expect(formatSeconds(0.0005)).toBe("0.5 ms");
    expect(formatSeconds(0.02)).toBe("20 ms");
    expect(formatSeconds(0.25)).toBe("250 ms");
    expect(formatSeconds(1.5)).toBe("1.5 s");
    expect(formatSeconds(4)).toBe("4 s");
    expect(formatClock(64.5124)).toBe("1:04.512");
    expect(formatClock(0)).toBe("0:00.000");
    expect(formatClock(-0.25)).toBe("−0:00.250");
    expect(formatSamples(218.24)).toBe("218.2 samples");
    expect(formatSamples(48000)).toBe("48000 samples");
  });

  test("formats dBFS readouts", () => {
    expect(formatDbfs(1)).toBe("0.0");
    expect(formatDbfs(0.5)).toBe(`${minus}6.0`);
    expect(formatDbfs(0)).toBe(`${minus}∞`);
    expect(formatDbfs(1e-5)).toBe(`${minus}100.0`);
    expect(formatDbfs(1.2e-6)).toBe(`${minus}118.4`);
    expect(formatDbfs(1e-6)).toBe(`${minus}∞`); // −120 dB
    expect(formatDbfs(9.9958e-7)).toBe(`${minus}∞`); // Rounds to −120.0.
    expect(formatDbfs(1e-8)).toBe(`${minus}∞`);
    expect(formatDbfs(Number.NaN)).toBe("—");
    expect(formatDbfs(Infinity)).toBe("—");
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
    expect(formatGain(-6.02)).toBe(`${minus}6.0 dB`);
    expect(formatGain(3)).toBe("+3.0 dB");
    expect(formatGain(-99.9)).toBe(`${minus}99.9 dB`);
    expect(formatGain(-100)).toBe(`${minus}100.0 dB`);
    expect(formatGain(-139.9)).toBe(`${minus}139.9 dB`);
    expect(formatGain(-140)).toBe(`${minus}∞ dB`);
    expect(formatGain(-332.2)).toBe(`${minus}∞ dB`);
    expect(formatGain(-151.8)).toBe(`${minus}∞ dB`);
    expect(formatGain(Number.NaN)).toBe("—");
  });

  test("parses typed quantities in their field's units only", () => {
    expect(parseQuantity("20 ms", "time")).toBeCloseTo(0.02);
    expect(parseQuantity("0.5s", "time")).toBe(0.5);
    expect(parseQuantity("500 us", "time")).toBeCloseTo(0.0005);
    expect(parseQuantity("20", "time", 0.001)).toBeCloseTo(0.02);
    expect(parseQuantity("-12 dB", "level")).toBe(-12);
    expect(parseQuantity("440 Hz", "pitch")).toBe(440);
    expect(parseQuantity(`${minus}0.25`, "number")).toBe(-0.25);
    expect(parseQuantity("0,5", "number")).toBe(0.5);
    expect(parseQuantity("fast", "number")).toBeUndefined();
    expect(parseQuantity("20 ms", "level")).toBeUndefined();
    expect(parseQuantity("3 dB", "time")).toBeUndefined();
    expect(parseQuantity("1 s", "number")).toBeUndefined();
  });

  test("rounds before choosing a unit, never in exponent notation", () => {
    expect(formatSeconds(0.9996)).toBe("1 s");
    expect(formatSeconds(0.0009996)).toBe("1 ms");
    expect(formatSeconds(-0.25)).toBe(`${minus}250 ms`);
    expect(formatFrequency(999.97)).toBe("≈ 1000");
    expect(formatFrequency(999.94)).toBe("≈ 999.9");
    expect(formatSamples(9999.96)).toBe("10000 samples");
    expect(formatDbfs(0.999)).toBe("0.0");
    expect(formatDbfs(0.99)).toBe(`${minus}0.1`);
    expect(formatOffset(1000 / 48000)).toBe("+0.021 ms"); // One sample at 48 kHz.
    expect(formatOffset(1000 / 384000)).toBe("+0.0026 ms"); // One sample at 384 kHz.
    expect(formatOffset(-1.5)).toBe(`${minus}1.5 ms`);
    expect(formatOffset(0.996)).toBe("+1.0 ms");
    expect(formatOffset(0)).toBe("0.0 ms");
  });

  test("reads back what each field shows", () => {
    for (const seconds of [0.0005, 0.02, 0.25, 1.5, 4]) {
      expect(parseQuantity(formatSeconds(seconds), "time")).toBeCloseTo(
        seconds,
        9,
      );
    }
    for (const db of [-60, -7.5, 0, 6]) {
      expect(parseQuantity(formatDb(db), "level")).toBe(db);
    }
    for (const hertz of [415, 440, 441.5]) {
      expect(parseQuantity(`${formatPitch(hertz)} Hz`, "pitch")).toBe(hertz);
    }
    expect(parseQuantity(signed(-0.25, 3), "number")).toBe(-0.25);
  });
});

describe("steps", () => {
  test("steps finely between 1-2-5 values with Shift", () => {
    expect(stepFine(spanSteps, 0.02, 1)).toBe(0.0212);
    expect(stepFine(spanSteps, 0.02, -1)).toBe(0.0189);
    expect(stepFine(spanSteps, 4, 1)).toBe(4);
    expect(stepFine(spanSteps, 0.0005, -1)).toBe(0.0005);
    expect(stepFine(holdOffSteps, 0, 1)).toBe(0.001);
    expect(stepFine(holdOffSteps, 0.001, -1)).toBeLessThan(0.001);
  });

  test("moves a fine step from any value in the range, and stays at zero going down", () => {
    for (const steps of [spanSteps, holdOffSteps]) {
      for (const value of steps.filter((step) => step > 0)) {
        for (const delta of [-1, 1]) {
          const next = stepFine(steps, value, delta);
          const atEnd = value === (delta < 0 ? steps[0] : steps.at(-1));
          expect(next === value).toBe(atEnd);
        }
      }
    }
    expect(stepFine(holdOffSteps, 0, -1)).toBe(0);
  });

  test("follows the 1-2-5 span sequence from 0.5 ms to 4 s", () => {
    expect(spanSteps[0]).toBe(0.0005);
    expect(spanSteps.at(-1)).toBe(4);
    expect(stepThrough(spanSteps, 0.02, 1)).toBe(0.05);
    expect(stepThrough(spanSteps, 0.02, -2)).toBe(0.005);
    expect(stepThrough(spanSteps, 0.023, 1)).toBe(0.05); // From the nearest step.
    expect(stepThrough(spanSteps, 4, 3)).toBe(4);
    // A hold-off typed between zero and the first step is nearest zero: up goes to 1 ms.
    expect(stepThrough(holdOffSteps, 0.0004, 1)).toBe(0.001);
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
    partial: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    panReach: 0,
    invalidSamples: 0,
    invalidLanes: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
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

  // Normal and Single draw only on a trigger, so they say they are waiting for one: before the
  // first frame, and once frames stop. Auto and Free draw without one.
  test.each(Object.keys(TriggerMode))(
    "in %s, says it is waiting only when frames are missing and the mode needs a trigger",
    (mode) => {
      const waits = mode === "normal" || mode === "single";
      const texts = (frameNow: Frame | undefined, seconds: number) =>
        statusChips(frameNow, mode, seconds, 0).map((chip) => chip.text);
      expect(
        statusChips(undefined, mode, 0, 0).find(
          (chip) => chip.text === "Waiting",
        ),
      ).toEqual(waits ? { text: "Waiting", tone: "warn" } : undefined);
      expect(texts(undefined, 0).at(-1)).toBe(mode);
      expect(
        texts(frame({ triggered: true }), waitingAfterSeconds * 2).includes(
          "Waiting",
        ),
      ).toBe(waits);
      expect(
        texts(frame({ triggered: true }), waitingAfterSeconds / 2),
      ).toEqual(["Trig’d", mode]);
      // A fresh frame drawn without a trigger, as Auto and Free draw: the mode alone.
      if (!waits) {
        expect(texts(frame({}), 0)).toEqual([mode]);
      }
    },
  );

  test.each([
    { state: "triggered", overrides: { triggered: true } },
    { state: "synced", overrides: { synced: true } },
    { state: "rolling", overrides: { roll: true } },
    { state: "frozen", overrides: { frozen: true } },
  ])(
    "warns of samples that are not finite whatever the display does ($state)",
    ({ overrides }) => {
      for (const invalid of [{ invalidSamples: 3 }, { invalidLanes: 0b100 }]) {
        expect(
          statusChips(frame({ ...overrides, ...invalid }), "auto", 0, 0).at(-1),
        ).toEqual({ text: "Invalid samples", tone: "warn" });
      }
      expect(
        statusChips(frame(overrides), "auto", 0, 0).map((chip) => chip.text),
      ).not.toContain("Invalid samples");
    },
  );

  test.each([0, 1, 2, 31])("reads lane %i's invalid-samples bit", (index) => {
    const marked = frame({ invalidLanes: (2 ** index) >>> 0 });
    expect(laneHoldsInvalid(marked, index)).toBe(true);
    expect(laneHoldsInvalid(marked, index === 0 ? 1 : 0)).toBe(false);
    expect(laneHoldsInvalid(frame({ invalidLanes: 0 }), index)).toBe(false);
  });

  test("shows sync, a shortened span, and a sweep still filling", () => {
    expect(statusChips(frame({ synced: true }), "auto", 0, 0)).toEqual([
      { text: "Sync", tone: "ok" },
    ]);
    expect(
      statusChips(frame({ synced: true, partial: true }), "auto", 0, 0)[1],
    ).toEqual({ text: "Filling", tone: "neutral" });
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
      { text: "−120 ms", tone: "neutral" },
    ]);
    expect(
      statusChips(frame({ frozen: true, partial: true }), "auto", 5, 0)[1],
    ).toEqual({ text: "Partial", tone: "neutral" });
  });
});
