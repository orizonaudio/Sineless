// The frame barcode's decoder and counts (scripts/frame-barcode): a row read back from greys, and
// per refresh what a strip shows, from recordings simulated here: a producer draws pictures for
// refreshes, the screen shows the newest one due, and a recorder samples each refresh.
import { describe, expect, test } from "vitest";

import {
  barcodeDataBits,
  barcodeTicksPerMillisecond,
} from "../../protocol/src/limits.ts";
import {
  barcodeRow,
  decodeRow,
  decodeStrip,
  type StripRead,
} from "../../ui/src/render/frame-barcode.ts";
import {
  baselinePictures,
  countRefreshes,
  gapPeriods,
  type Sample,
} from "../frame-barcode/stats.ts";

const range = 2 ** barcodeDataBits;

function greys(value: number, white = 1, black = 0): number[] {
  return barcodeRow(value).map((cell) => (cell ? white : black));
}

describe("a row read back from its cells' greys", () => {
  test.each([0, 1, 255, 4096, 12345, range - 1])("reads %i", (value) => {
    expect(decodeRow(greys(value))).toEqual({ value });
  });

  test("reads through colour management, compression and noise", () => {
    const noisy = greys(43690, 0.82, 0.31).map(
      (grey, index) => grey + 0.04 * Math.sin(index * 7.3),
    );
    expect(decodeRow(noisy)).toEqual({ value: 43690 });
  });

  test("refuses a row it cannot be sure of, never guessing", () => {
    const flipped = greys(12345);
    flipped[5] = 1 - (flipped[5] ?? 0);
    expect(decodeRow(flipped)).toEqual({ unreadable: "parity" });
    const crossed = greys(12345);
    crossed[7] = 0.5; // A cursor line across the cell.
    expect(decodeRow(crossed)).toEqual({
      unreadable: "cell neither white nor black",
    });
    expect(decodeRow(greys(12345, 0.55, 0.45))).toEqual({
      unreadable: "references",
    });
    const clipped = greys(12345);
    clipped[clipped.length - 1] = 1;
    expect(decodeRow(clipped)).toEqual({ unreadable: "end" });
    expect(decodeRow(greys(12345).slice(0, -1))).toEqual({
      unreadable: "cell count",
    });
  });

  test("a strip is its picture and the time it was drawn for", () => {
    expect(decodeStrip(greys(7), greys(900))).toEqual({
      picture: 7,
      drawnFor: 900,
    });
    expect(decodeStrip(greys(7), greys(900, 0.5, 0.5))).toEqual({
      unreadable: "drawn for: references",
    });
  });
});

// ----- Simulated recordings -----

const period = 1000 / 60;

interface Scenario {
  readonly refreshes: number;
  // Pipeline depth: refreshes between the one a picture is drawn for and the one it shows at.
  readonly depth?: number;
  // Whether a picture is drawn for refresh r (making).
  readonly drawn?: (r: number) => boolean;
  // Extra refreshes before the picture drawn for r shows (presentation).
  readonly delay?: (r: number) => number;
  // The recorder's view of refresh s.
  readonly missing?: (s: number) => boolean;
  readonly unreadable?: (s: number) => boolean;
  readonly jitter?: (s: number) => number;
  // The first picture's number, the producer's clock at refresh 0, and the recorder's.
  readonly firstPicture?: number;
  readonly producerMs?: number;
  readonly recorderMs?: number;
  // A picture number the count jumps back to at refresh `at` (the editor reopened).
  readonly restart?: { readonly at: number; readonly picture: number };
}

interface Drawn {
  readonly picture: number;
  readonly drawnFor: number;
  readonly shows: number;
}

// What one strip shows at each refresh.
function stripTimeline(scenario: Scenario): (StripRead | undefined)[] {
  const depth = scenario.depth ?? 2;
  const drawn: Drawn[] = [];
  let picture = scenario.firstPicture ?? 0;
  for (let r = 0; r < scenario.refreshes; r++) {
    if (scenario.restart?.at === r) {
      picture = scenario.restart.picture;
    }
    if (scenario.drawn?.(r) ?? true) {
      const time = (scenario.producerMs ?? 3_600_000) + r * period;
      drawn.push({
        picture: picture % range,
        drawnFor: Math.round(time * barcodeTicksPerMillisecond) % range,
        shows: r + depth + (scenario.delay?.(r) ?? 0),
      });
      picture++;
    }
  }
  return Array.from({ length: scenario.refreshes }, (_, s) => {
    let shown: Drawn | undefined;
    drawn.forEach((each, index) => {
      if (
        each.shows <= s &&
        (shown === undefined || index > drawn.indexOf(shown))
      ) {
        shown = each;
      }
    });
    return shown === undefined
      ? undefined
      : { picture: shown.picture, drawnFor: shown.drawnFor };
  });
}

function record(
  scenario: Scenario,
  strips: (StripRead | undefined)[][],
  idle = false,
): Sample[] {
  const samples: Sample[] = [];
  const start = (scenario.depth ?? 2) + 1;
  for (let s = start; s < scenario.refreshes; s++) {
    if (scenario.missing?.(s) === true) {
      continue;
    }
    const reads = strips.map((timeline) =>
      scenario.unreadable?.(s) === true
        ? { unreadable: "hidden" }
        : (timeline[s] ?? { unreadable: "none" }),
    );
    const previous = strips.map((timeline) => timeline[s - 1]);
    const unchanged =
      idle &&
      reads.every(
        (read, index) =>
          "picture" in read &&
          JSON.stringify(read) === JSON.stringify(previous[index]),
      );
    samples.push({
      displayTime:
        (scenario.recorderMs ?? 98_765.4) +
        s * period +
        (scenario.jitter?.(s) ?? 0),
      idle: unchanged,
      strips: unchanged ? [] : reads,
    });
  }
  return samples;
}

function counts(scenario: Scenario) {
  const result = countRefreshes(record(scenario, [stripTimeline(scenario)]));
  const strip = result.strips[0];
  expect(strip).toBeDefined();
  return strip ?? countRefreshes([]).strips[0];
}

describe("counting what the screen showed", () => {
  test("steady: every refresh a new picture, on time", () => {
    const strip = counts({ refreshes: 600 });
    expect(strip).toMatchObject({
      newLate: 0,
      newUnknown: 0,
      repeatNothingDrawn: 0,
      repeatPresentedLate: 0,
      repeatUnknown: 0,
      lost: 0,
      unknownAcrossGaps: 0,
    });
    expect(strip?.newOnTime).toBe(600 - 3 - 1);
  });

  test("a refresh nothing was drawn for repeats as making, not presentation", () => {
    const strip = counts({ refreshes: 600, drawn: (r) => r % 10 !== 5 });
    expect(strip?.repeatNothingDrawn).toBeGreaterThanOrEqual(58);
    expect(strip?.repeatPresentedLate).toBe(0);
    expect(strip?.lost).toBe(0);
    expect(strip?.newLate).toBe(0);
  });

  test("a picture shown a refresh late: its refresh repeats as presentation, and it is late", () => {
    const strip = counts({
      refreshes: 600,
      delay: (r) => (r % 10 === 5 ? 1 : 0),
      drawn: (r) => r % 10 !== 6,
    });
    expect(strip?.repeatPresentedLate).toBeGreaterThanOrEqual(58);
    expect(strip?.repeatNothingDrawn).toBe(0);
    expect(strip?.newLate).toBeGreaterThanOrEqual(58);
  });

  test("a picture drawn and overtaken is lost, and its refresh repeats as presentation", () => {
    const strip = counts({
      refreshes: 600,
      delay: (r) => (r % 10 === 5 ? 1 : 0),
    });
    expect(strip?.lost).toBeGreaterThanOrEqual(58);
    expect(strip?.repeatPresentedLate).toBeGreaterThanOrEqual(58);
    expect(strip?.repeatNothingDrawn).toBe(0);
  });

  test("a picture just within half a period of the baseline is on time, just beyond it late", () => {
    const within = counts({
      refreshes: 600,
      jitter: (s) => (s === 300 ? 0.4 * period : 0),
    });
    expect(within?.newLate).toBe(0);
    const beyond = counts({
      refreshes: 600,
      jitter: (s) => (s === 300 ? 0.6 * period : 0),
    });
    expect(beyond?.newLate).toBe(1);
  });

  test("when most pictures are late, the late ones still read late", () => {
    // Of every five refreshes: one picture on time, two shown a refresh late, two with nothing.
    const strip = counts({
      refreshes: 900,
      delay: (r) => (r % 5 === 1 || r % 5 === 3 ? 1 : 0),
      drawn: (r) => r % 5 !== 2 && r % 5 !== 4,
    });
    const late = strip?.newLate ?? 0;
    const onTime = strip?.newOnTime ?? 0;
    expect(late).toBeGreaterThan(1.8 * onTime);
  });

  test("a deeper pipeline from some point reads late only within the window, then steady", () => {
    const strip = counts({
      refreshes: 1200,
      delay: (r) => (r >= 600 ? 1 : 0),
    });
    expect(strip?.newLate ?? 0).toBeLessThanOrEqual(61);
    expect(strip?.newOnTime ?? 0).toBeGreaterThan(1000);
  });

  test("too few pictures around one to set a baseline: unknown, never on time or late", () => {
    const sparse = counts({ refreshes: 900, drawn: (r) => r % 5 === 0 });
    expect(sparse?.newOnTime).toBe(0);
    expect(sparse?.newLate).toBe(0);
    expect(sparse?.newUnknown).toBeGreaterThan(0);
    // Every fourth refresh gives about baselinePictures in the window: judged.
    const enough = counts({ refreshes: 900, drawn: (r) => r % 4 === 0 });
    expect(enough?.newOnTime).toBeGreaterThan(0);
    expect(baselinePictures).toBeGreaterThan(121 / 5);
  });
});

describe("what the recorder could not see", () => {
  test("jitter below the gap is no gap; a step beyond it is, and its refreshes are unknown", () => {
    const jittered = counts({
      refreshes: 600,
      jitter: (s) => (s === 300 ? (gapPeriods - 1 - 0.1) * period : 0),
    });
    expect(jittered?.unknownAcrossGaps).toBe(0);
    const gap = counts({
      refreshes: 600,
      missing: (s) => s === 300,
      jitter: (s) => (s === 301 ? -(2 - gapPeriods - 0.1) * period : 0),
    });
    expect(gap?.unknownAcrossGaps).toBe(1);
  });

  test("a picture skipped inside a recorder gap is unknown, not lost", () => {
    const strip = counts({ refreshes: 600, missing: (s) => s === 300 });
    expect(strip?.unknownAcrossGaps).toBe(1);
    expect(strip?.lost).toBe(0);
  });

  test("a hidden stretch is unreadable, and the jump across it is not lost", () => {
    const strip = counts({
      refreshes: 900,
      unreadable: (s) => s >= 300 && s < 420,
    });
    expect(strip?.unreadable).toBe(120);
    expect(strip?.lost).toBe(0);
    expect(strip?.newLate).toBe(0);
  });

  test("an idle recorder sample is a repeat on screen", () => {
    const scenario: Scenario = { refreshes: 600, drawn: (r) => r % 10 !== 5 };
    const result = countRefreshes(
      record(scenario, [stripTimeline(scenario)], true),
    );
    expect(result.strips[0]?.idle).toBeGreaterThanOrEqual(58);
    expect(result.strips[0]?.repeatNothingDrawn).toBeGreaterThanOrEqual(58);
  });
});

describe("counts that wrap, restart or pause", () => {
  test("the picture count and the drawn-for time wrap without a skip, at any uptime", () => {
    const strip = counts({
      refreshes: 900,
      firstPicture: range - 300,
      producerMs: 5 * 86_400_000 + 0.37,
      recorderMs: 12_345_678.9,
      jitter: (s) => 0.3 * Math.sin(s),
    });
    expect(strip).toMatchObject({ lost: 0, newLate: 0, restarts: 0 });
  });

  test("a count starting again is a restart, not tens of thousands of skips", () => {
    const strip = counts({
      refreshes: 900,
      restart: { at: 400, picture: 0 },
      firstPicture: 40_000,
    });
    expect(strip?.restarts).toBe(1);
    expect(strip?.lost).toBe(0);
  });

  test.each([8.0, 8.4, 20, 40])(
    "a pause of %f s with nothing drawn, then pictures again: repeats are making, lateness right on both sides",
    (seconds) => {
      const pause = Math.round((seconds * 1000) / period);
      const strip = counts({
        refreshes: 600 + pause + 600,
        drawn: (r) => r < 600 || r >= 600 + pause,
      });
      expect(strip?.newLate).toBe(0);
      expect(strip?.lost).toBe(0);
      expect(strip?.repeatPresentedLate).toBe(0);
      expect(strip?.repeatNothingDrawn).toBe(pause);
    },
  );
});

test("two strips stalling at the same refreshes are counted together", () => {
  const stall = (r: number) => (r % 10 === 5 ? 1 : 0);
  const a: Scenario = { refreshes: 600, delay: stall };
  const b: Scenario = { refreshes: 600, delay: stall, firstPicture: 9000 };
  const result = countRefreshes(
    record(a, [stripTimeline(a), stripTimeline(b)]),
  );
  expect(result.strips).toHaveLength(2);
  expect(result.repeatedTogether).toBeGreaterThanOrEqual(58);
});
