// A page whose rendering stops gets no animation frames, as the CI machine's display stalling
// leaves it: the start then stops within its budget, saying so, instead of waiting for frames
// that never come until the test's 30 s timeout. On CI the test is skipped; off CI, and outside a
// test, it fails.
import {
  afterAll,
  afterEach,
  beforeAll,
  describe,
  expect,
  test,
  vi,
} from "vitest";

import {
  frame,
  setCiForTests,
  setFrameBudgetForTests,
  startApp,
} from "./harness.ts";

const realFrame = window.requestAnimationFrame.bind(window);

// Animation frames as a page whose rendering stops after `frames` of them gets them.
function framesStopAfter(frames: number): void {
  let given = 0;
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) =>
    given++ < frames ? realFrame(callback) : 0,
  );
}

// Animation frames as a page whose rendering stalls for `ms` gets them: those asked meanwhile are
// held, as a stalled page holds them, and run when it resumes.
function framesStallFor(ms: number): void {
  const held: FrameRequestCallback[] = [];
  let stalled = true;
  setTimeout(() => {
    stalled = false;
    for (const callback of held.splice(0)) {
      realFrame(callback);
    }
  }, ms);
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => {
    if (stalled) {
      held.push(callback);
      return 0;
    }
    return realFrame(callback);
  });
}

afterEach(() => {
  vi.unstubAllGlobals();
  setFrameBudgetForTests();
  setCiForTests();
});

describe("a start whose page stops rendering, off CI", () => {
  test("stops within its budget when no frame comes, saying why", async () => {
    setCiForTests(false);
    setFrameBudgetForTests(300);
    framesStopAfter(0);
    const started = performance.now();
    await expect(startApp()).rejects.toThrow(
      /no animation frame in the ticks the previous app asked for \(0 before it stopped; page (visible|hidden), (focused|not focused), \d+ × \d+\): its rendering stalled/,
    );
    expect(performance.now() - started).toBeLessThan(5000);
  });

  test("stops when frames come and then stop, saying how many came", async () => {
    setCiForTests(false);
    setFrameBudgetForTests(300);
    framesStopAfter(2);
    await expect(startApp()).rejects.toThrow(/\(2 before it stopped;/);
  });

  test("starts when frames come at one time, as held frames run in a burst", async () => {
    setCiForTests(false);
    setFrameBudgetForTests(5000);
    vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) =>
      realFrame(() => {
        callback(1000);
      }),
    );
    await startApp();
  });

  test("a frame waited for in a test stops when none comes", async () => {
    setCiForTests(false);
    framesStopAfter(0);
    await expect(frame("the grip's frame", 300)).rejects.toThrow(
      /no animation frame in the grip's frame \(page /,
    );
  });
});

describe("a start whose page stops rendering, on CI", () => {
  // eslint-disable-next-line no-empty-pattern -- vitest reads a hook's fixtures from this pattern.
  afterAll(({}, suite) => {
    // A skip's note is on the result, which vitest's types leave out.
    const outcomes = suite.tasks.map((task) => [
      task.result?.state,
      (task.result as { note?: string } | undefined)?.note
        ?.match(/no animation frame in [^(]+/)?.[0]
        .trim(),
    ]);
    expect(outcomes).toEqual([
      ["skip", "no animation frame in the ticks the previous app asked for"],
      ["skip", "no animation frame in the ticks the previous app asked for"],
      ["pass", undefined],
      ["skip", "no animation frame in the grip's frame"],
    ]);
  });

  test("is skipped when no frame comes before the app starts", async () => {
    setCiForTests(true);
    setFrameBudgetForTests(300);
    framesStopAfter(0);
    await startApp();
  });

  test("is skipped when frames come and then stop", async () => {
    setCiForTests(true);
    setFrameBudgetForTests(300);
    framesStopAfter(2);
    await startApp();
  });

  test("starts when the frames come back within the budget", async () => {
    setCiForTests(true);
    setFrameBudgetForTests(600);
    framesStallFor(300);
    await startApp();
  });

  test("is skipped when a frame waited for in a test does not come", async () => {
    setCiForTests(true);
    framesStopAfter(0);
    await frame("the grip's frame", 300);
  });
});

// A start in a once-per-file setup has no test to skip: it fails with the same note, on CI too.
describe("a start whose page stops rendering, outside a test", () => {
  let failure: unknown;

  beforeAll(async () => {
    setCiForTests(true);
    setFrameBudgetForTests(300);
    framesStopAfter(0);
    try {
      await startApp();
    } catch (error) {
      failure = error;
    }
  });

  test("fails with the stall's note", () => {
    expect(String(failure)).toMatch(/its rendering stalled/);
  });
});
