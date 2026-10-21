// Rules every formatter keeps, run over all of them and over the values that broke them before:
// silence, denormals, values just under a unit boundary, exact cancellation, and values that are
// not numbers. And every typed field reads back what it shows, walked from the field table.
import { describe, expect, test } from "vitest";

import { settings } from "../../protocol/src/settings.ts";
import { maxLanes } from "../../protocol/src/limits.ts";
import { levelReadout } from "../src/view/cursors.ts";
import { numberFields } from "../src/view/fields.ts";
import {
  formatDbfs,
  formatFrequency,
  formatGain,
  formatOffset,
  formatSamples,
  formatSeconds,
  formatClock,
  parseQuantity,
} from "../src/view/format.ts";
import * as format from "../src/view/format.ts";
import { laneStyles, sumLaneColour } from "../src/view/lanes.ts";

const minus = "−";

// What no readout may show: a number JavaScript did not format for people, or a negative zero.
function expectReadable(text: string): void {
  expect(text).not.toMatch(/NaN|Infinity|undefined|e[+-]\d/u);
  expect(text).not.toMatch(/(^|[^\d.])[-−]0(\.0+)?(?!\d|\.\d*[1-9])/u);
  expect(text).not.toContain("-"); // Negative numbers use a true minus sign.
}

const notNumbers = [Number.NaN, Infinity];

// Level formatters, from a linear amplitude.
const linearFormatters = {
  formatDbfs,
  levelCursor: (linear: number) =>
    levelReadout({ first: linear, second: 0.5 })[0] ?? "",
};
const linearValues = [
  0, 1e-45, 1e-10, 1e-6, 9.9958e-7, 1.2e-6, 1e-5, 0.5, 0.999, 1, 2,
];

describe.each(Object.entries(linearFormatters))("%s", (_, format) => {
  test.each(linearValues)("is readable at %s", (value) => {
    expectReadable(format(value));
  });
  test.each([0, 1e-45, 1e-10, 1e-6])(
    "reads −∞ at or below the floor (%s)",
    (value) => {
      expect(format(value)).toContain(`${minus}∞`);
    },
  );
  test("reads 0.0, not −0.0, just under full scale", () => {
    expect(format(0.999)).toContain("0.0");
  });
});

describe("formatBeats", () => {
  test.each([
    [1, "1 beat"],
    [0.25, "0.25 beats"],
    [0, "0 beats"],
    [6.1e-7, "0.00000061 beats"],
    [0.99996, "1 beat"],
  ])("reads %s quarter notes as %s", (quarterNotes, text) => {
    expect(format.formatBeats(quarterNotes)).toBe(text);
  });
});

describe("formatDbDifference", () => {
  test.each([
    [6, "6.0 dB"],
    [0.04, "0.0 dB"],
    [12.96, "13.0 dB"],
  ])("reads %s dB as %s", (db, text) => {
    expect(format.formatDbDifference(db)).toBe(text);
  });
});

describe("formatGain", () => {
  test.each([
    -Infinity,
    -332.2,
    -151.8,
    -140,
    -139.96,
    -100,
    -0.04,
    0,
    0.04,
    3,
    30,
  ])("is readable at %s dB", (db) => {
    expectReadable(formatGain(db));
  });
  test.each([-Infinity, -332.2, -151.8, -140, -139.96])(
    "reads −∞ at %s dB",
    (db) => {
      expect(formatGain(db)).toBe(`${minus}∞ dB`);
    },
  );
});

// Every formatter the module exports, given something that is not a number: taken from the
// module itself, so a new formatter is checked as it is added.
const allFormatters = Object.entries(format).filter(
  (entry): entry is [string, (value: number) => string] =>
    entry[0].startsWith("format") && typeof entry[1] === "function",
);
test("finds the formatters", () => {
  expect(allFormatters.length).toBeGreaterThanOrEqual(10);
});
describe.each(allFormatters)("%s", (_, formatValue) => {
  test.each(notNumbers)("shows a dash for %s", (value) => {
    expect(formatValue(value)).toBe("—");
  });
});

// Values that round to zero from below show no negative zero, in every formatter whose values
// can be negative: a Compare offset of a track a hair early, a time or project time just before
// zero, a level or gain just under 0 dB. Taken from the module, so a new formatter is checked
// unless it is named here as one whose values are never negative.
const neverNegative = new Set([
  "formatSamples",
  "formatFrequency",
  "formatPitch",
]);
const signedFormatters = allFormatters.filter(
  ([name]) => !neverNegative.has(name),
);
test("finds the signed formatters", () => {
  expect(signedFormatters.length).toBeGreaterThanOrEqual(6);
});
describe.each(signedFormatters)("%s near zero", (_, formatValue) => {
  test.each([0, -0, -1e-9, -0.0004])("is readable at %s", (value) => {
    expectReadable(formatValue(value));
  });
});

// And values that do not round to zero keep a true minus.
test.each([
  [formatOffset, -0.42, `${minus}0.42 ms`],
  [formatOffset, -3.2, `${minus}3.2 ms`],
  [formatClock, -1.5, `${minus}0:01.500`],
  [formatClock, -0.0004, "0:00.000"],
] as const)("%o of %s shows %s", (formatValue, value, shown) => {
  expect(formatValue(value)).toBe(shown);
});

// A field reads what the readouts print, which is a true minus, and only a number with its unit.
test.each([
  ["−12 dB", "level", -12],
  ["−0.25", "number", -0.25],
  ["20 ms", "time", 0.02],
  ["20 ms ", "time", 0.02],
  ["2e1 ms", "time", 0.02],
  ["1.5e-3 s", "time", 0.0015],
] as const)("reads %s", (text, quantity, value) => {
  expect(parseQuantity(text, quantity)).toBeCloseTo(value, 9);
});
test.each(["20 ms junk", "junk 20 ms", "2 0 ms", "20 ms ms"])(
  "rejects %s",
  (text) => {
    expect(parseQuantity(text, "time")).toBeUndefined();
  },
);

// Values on either side of each unit or precision switch.
describe("unit boundaries", () => {
  test.each([
    [formatSeconds, 0.0009994, "0.999 ms"],
    [formatSeconds, 0.0009996, "1 ms"],
    [formatSeconds, 0.9994, "999 ms"],
    [formatSeconds, 0.9996, "1 s"],
    [formatFrequency, 999.94, "≈ 999.9"],
    [formatFrequency, 999.97, "≈ 1000"],
    [formatSamples, 9999.94, "9999.9 samples"],
    [formatSamples, 9999.96, "10000 samples"],
    [formatOffset, 0.994, "+0.99 ms"],
    [formatOffset, 0.996, "+1.0 ms"],
  ] as const)("%o of %s shows %s", (format, value, shown) => {
    expect(format(value)).toBe(shown);
    expectReadable(format(value));
  });
});

// Every number field reads back what it shows, at its setting's minimum, maximum and default.
const fieldCases = Object.entries(numberFields).flatMap(([name, field]) => {
  const setting = settings[name as keyof typeof settings];
  if (setting.kind !== "number") {
    return [];
  }
  return [setting.minimum, setting.maximum, setting.defaultValue].map(
    (value) => ({
      name,
      field,
      value,
    }),
  );
});
describe.each(fieldCases)("$name at $value", ({ field, value }) => {
  test("reads back what it shows", () => {
    const shown = field.format(value);
    expect(parseQuantity(shown, field.quantity, field.bareScale)).toBeCloseTo(
      value,
      9,
    );
    expectReadable(shown);
  });
});

// Lanes without a host colour, from one up to the lane limit.
const laneCounts = Array.from({ length: maxLanes }, (_, index) => index + 1);
describe.each(laneCounts)("%i lanes without host colours", (count) => {
  const styles = laneStyles(
    Array.from({ length: count }, (_, index) => ({
      id: index,
      name: "",
      colour: null,
      own: index === 0,
      compared: true,
    })),
  );
  test("each has its own colour, none the sum lane's", () => {
    const colours = styles.map((style) => style.colour);
    expect(new Set(colours).size).toBe(count);
    expect(colours).not.toContain(sumLaneColour);
  });
});
