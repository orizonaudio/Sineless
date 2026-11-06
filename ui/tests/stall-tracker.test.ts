import { describe, expect, test } from "vitest";

import {
  stallLimitMs,
  StallTracker,
  type WatchedProject,
} from "./app/stall-tracker.ts";

const limitMs = 120_000;

function tracker(projects: Record<string, number>): StallTracker {
  const tracked = new StallTracker();
  tracked.start(
    new Map<string, WatchedProject>(
      Object.entries(projects).map(([name, files]) => [
        name,
        { files, limitMs },
      ]),
    ),
  );
  return tracked;
}

// A browser project whose page stops sending anything is stalled once its limit has passed, and
// named with what it did last, wherever in a file it stopped: no other check would end the run.
describe("a browser project's progress", () => {
  test("a green run of two projects, one ending minutes before the other, never stalls", () => {
    const run = tracker({ webkit: 2, chromium: 1 });
    run.progress("chromium", "a.test.ts was queued", 0);
    run.fileEnded("chromium", "a.test.ts ended", 1000);
    run.progress("webkit", "a.test.ts was queued", 1000);
    run.fileEnded("webkit", "a.test.ts ended", 60_000);
    run.progress("webkit", "b.test.ts was queued", 61_000);
    run.progress("webkit", 'test "b" started', 100_000);
    run.fileEnded("webkit", "b.test.ts ended", 150_000);
    expect(run.check(600_000)).toEqual([]);
  });

  test.each([
    ["at a file's load", "page-barcode.test.ts was queued"],
    ["in a test", 'test "draws the strip" started'],
    ["in a hook", 'beforeEach of "draws the strip" started'],
  ])("is stalled %s, named by its last event, once", (_, what) => {
    const run = tracker({ webkit: 2 });
    run.progress("webkit", what, 1000);
    expect(run.check(1000 + limitMs - 1)).toEqual([]);
    const [stall, ...more] = run.check(1000 + limitMs);
    expect(more).toEqual([]);
    expect(stall?.project).toBe("webkit");
    expect(stall?.report).toContain(`no progress for 120 s since ${what}`);
    expect(run.check(1000 + 2 * limitMs)).toEqual([]);
  });

  test("is stalled between files, after one ends and before the next is queued", () => {
    const run = tracker({ webkit: 9 });
    run.fileEnded("webkit", "field-pills.test.ts ended", 5000);
    const [stall] = run.check(5000 + limitMs);
    expect(stall?.report).toContain("since field-pills.test.ts ended");
  });

  test("only the silent project is stalled", () => {
    const run = tracker({ webkit: 2, chromium: 2 });
    run.progress("webkit", "a.test.ts was queued", 0);
    run.progress("chromium", "a.test.ts was queued", 0);
    run.progress("chromium", 'test "a" started', limitMs - 10);
    expect(run.check(limitMs).map((stall) => stall.project)).toEqual([
      "webkit",
    ]);
  });

  test("a project that has sent nothing yet, waiting behind the other, is not stalled", () => {
    const run = tracker({ webkit: 1, chromium: 1 });
    run.progress("chromium", "a.test.ts was queued", 0);
    run.fileEnded("chromium", "a.test.ts ended", 1000);
    expect(run.check(10 * limitMs)).toEqual([]);
  });

  test("nothing is stalled once the run has ended", () => {
    const run = tracker({ webkit: 2 });
    run.progress("webkit", "a.test.ts was queued", 0);
    run.stop();
    expect(run.check(10 * limitMs)).toEqual([]);
  });

  test("a project that is not watched (the unit tests) is ignored", () => {
    const run = tracker({ webkit: 1 });
    run.progress("unit", "x.test.ts was queued", 0);
    expect(run.check(10 * limitMs)).toEqual([]);
  });
});

// The limit follows the project's test timeout, which bounds every wait a healthy page makes; the
// variable sets another only when it is a positive number of seconds, so a typo cannot turn the
// watchdog off (Infinity) or fail every run at once (0).
describe("the limit", () => {
  test.each([
    [undefined, 30_000, 120_000],
    [undefined, 10_000, 40_000],
    [undefined, 0, 120_000],
    [undefined, Number.NaN, 120_000],
    ["20", 30_000, 20_000],
    [" 20 ", 30_000, 20_000],
    ["0.5", 30_000, 500],
    ["", 30_000, 120_000],
    ["20abc", 30_000, 120_000],
    ["0", 30_000, 120_000],
    ["-5", 30_000, 120_000],
    ["Infinity", 30_000, 120_000],
    ["1e999", 30_000, 120_000],
  ])(
    "with ORIZON_APP_STALL_SECONDS %j and a %d ms test timeout is %d ms",
    (override, timeout, expected) => {
      expect(stallLimitMs(override, timeout)).toBe(expected);
    },
  );
});
