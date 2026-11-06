// A page whose rendering stops gets no animation frames, as the CI machine's display stalling
// leaves it: the start then stops within its budget, saying so, instead of waiting for frames
// that never come until the test's 30 s timeout.
import { afterEach, describe, expect, test, vi } from "vitest";

import { setFrameBudgetsForTests, startApp } from "./harness.ts";

const realFrame = window.requestAnimationFrame.bind(window);

// Animation frames as a page whose rendering stops after `frames` of them gets them.
function framesStopAfter(frames: number): void {
  let given = 0;
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) =>
    given++ < frames ? realFrame(callback) : 0,
  );
}

describe("a start whose page stops rendering", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    setFrameBudgetsForTests();
  });

  test("stops within its budget when no frame comes, saying why", async () => {
    setFrameBudgetsForTests(300);
    framesStopAfter(0);
    const started = performance.now();
    await expect(startApp()).rejects.toThrow(
      /no animation frame in the ticks the previous app asked for \(0 before it stopped; page (visible|hidden), (focused|not focused), \d+ × \d+\): its rendering stalled/,
    );
    expect(performance.now() - started).toBeLessThan(5000);
  });

  test("stops when frames come and then stop, saying how many came", async () => {
    setFrameBudgetsForTests(300);
    framesStopAfter(2);
    await expect(startApp()).rejects.toThrow(/\(2 before it stopped;/);
  });
});
