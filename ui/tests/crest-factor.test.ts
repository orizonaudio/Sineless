// The Meter's crest factor, peak over RMS in dB, on what the engine measured for the signal
// corpus's known signals: a sine's peak stands 3 dB over its RMS, DC's not at all, and silence has
// none, since its levels read −∞. And no value either side gives none.
import { describe, expect, test } from "vitest";

import { levelFloorDbfs } from "../../protocol/src/limits.ts";
import { meterReadout } from "../src/view/cursors.ts";
import { crestFactorDb, formatDbDifference } from "../src/view/format.ts";

interface CorpusFrame {
  readonly readouts: {
    readonly peak: number | "NaN";
    readonly rms: number | "NaN";
  };
}

const corpus = import.meta.glob<CorpusFrame>(
  "../../engine/tests/corpus/Single_*.json",
  { eager: true, import: "default" },
);
const cases = Object.entries(corpus).map(([path, frame]) => {
  const name = /Single_(?<name>[^/]+)\.json$/u.exec(path)?.groups?.name ?? path;
  const value = (level: number | "NaN") =>
    level === "NaN" ? Number.NaN : level;
  return [name, value(frame.readouts.peak), value(frame.readouts.rms)] as const;
});

const crestOf = (peak: number, rms: number) =>
  formatDbDifference(crestFactorDb(peak, rms));
const sine = crestOf(Math.SQRT2, 1);

describe("crest factor on the signal corpus", () => {
  test("finds the corpus", () => {
    expect(cases.length).toBeGreaterThanOrEqual(10);
  });
  test.each(cases.filter(([name]) => name.startsWith("Sine")))(
    "%s, a sine, reads 3.0 dB",
    (_, peak, rms) => {
      expect(crestOf(peak, rms)).toBe("3.0 dB");
    },
  );
  test.each(cases.filter(([name]) => name.startsWith("Dc")))(
    "%s, DC, reads 0.0 dB",
    (_, peak, rms) => {
      expect(crestOf(peak, rms)).toBe("0.0 dB");
    },
  );
  test.each(cases.filter(([name]) => name.startsWith("Silence")))(
    "%s, silence, has none",
    (_, peak, rms) => {
      expect(crestOf(peak, rms)).toBe("—");
    },
  );
  test.each(cases.filter(([name]) => name.startsWith("Noise")))(
    "%s, noise, peaks further over its RMS than a sine",
    (_, peak, rms) => {
      expect(crestFactorDb(peak, rms)).toBeGreaterThan(
        crestFactorDb(Math.SQRT2, 1),
      );
    },
  );
});

// A sine at any level reads 3.0 dB, also where its peak and RMS, each rounded to 0.1 dB as the
// Meter shows them, lie 3.1 dB apart (a peak of −0.04 dBFS shows 0.0 over an RMS of −3.1).
const sineLevels = Array.from({ length: 601 }, (_, step) => -step / 100);
test.each(sineLevels)("a sine peaking at %s dBFS reads 3.0 dB", (peakDb) => {
  const peak = 10 ** (peakDb / 20);
  expect(crestOf(peak, peak / Math.SQRT2)).toBe("3.0 dB");
});

describe("crest factor with no level", () => {
  const atFloor = 10 ** (levelFloorDbfs / 20);
  test.each([
    ["no peak", Number.NaN, 0.5],
    ["no RMS", 0.5, Number.NaN],
    ["a peak at the level floor", atFloor, atFloor / 2],
    ["an RMS at the level floor", 0.5, atFloor],
  ])("%s has none", (_, peak, rms) => {
    expect(crestOf(peak, rms)).toBe("—");
  });
  test("a sine just above the floor still has one", () => {
    expect(crestOf(atFloor * 4, (atFloor * 4) / Math.SQRT2)).toBe(sine);
  });
});

// The Meter's rows, as the corner panel shows them.
test("the Meter reads Peak, RMS and Crest with their units", () => {
  expect(meterReadout(0.5, 0.5 / Math.SQRT2)).toEqual([
    { label: "Peak", value: "−6.0 dBFS" },
    { label: "RMS", value: "−9.0 dBFS" },
    { label: "Crest", value: "3.0 dB" },
  ]);
  expect(meterReadout(Number.NaN, Number.NaN)).toEqual([
    { label: "Peak", value: "—" },
    { label: "RMS", value: "—" },
    { label: "Crest", value: "—" },
  ]);
});
