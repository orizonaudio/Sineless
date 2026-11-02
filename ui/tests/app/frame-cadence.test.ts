// On a display faster than the full rate, the page holds each engine frame for the same number of
// ticks and asks the engine for the next only then: at 100 Hz, one frame every two ticks (#308).
import { afterEach, expect, test, vi } from "vitest";

import { ticksPerFrame } from "../../src/render/render-loop.ts";
import { startApp } from "./harness.ts";

afterEach(() => {
  vi.unstubAllGlobals();
});

test("asks the engine for a frame once every n ticks on a 100 Hz display", async () => {
  const rate = 100;
  const period = 1000 / rate;
  const pending: FrameRequestCallback[] = [];
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => {
    pending.push(callback);
    return pending.length;
  });
  let now = 0;
  const ticks = setInterval(() => {
    now += period;
    for (const callback of pending.splice(0)) {
      callback(now);
    }
  }, 2);
  const until = async (at: number) => {
    while (now < at) {
      await new Promise((resolve) => setTimeout(resolve, 2));
    }
  };
  try {
    // The engine answers each request at once, so the page always has its next frame.
    const app = await startApp({
      schedule: (work) => {
        work();
      },
    });
    await until(now + 2000); // Long enough to measure the display.
    const requests = vi.spyOn(app.transport, "requestFrame");
    const from = now;
    await until(from + 2000);
    const elapsedTicks = Math.round((now - from) / period);
    expect(requests.mock.calls.length).toBeGreaterThanOrEqual(
      Math.floor(elapsedTicks / ticksPerFrame(rate)) - 1,
    );
    expect(requests.mock.calls.length).toBeLessThanOrEqual(
      Math.ceil(elapsedTicks / ticksPerFrame(rate)) + 1,
    );
  } finally {
    clearInterval(ticks);
  }
});
