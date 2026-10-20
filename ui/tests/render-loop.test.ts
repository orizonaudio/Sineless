import { describe, expect, test } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import { RenderLoop } from "../src/render/render-loop.ts";

// A frame told apart from the others by its point count, `id`; the loop never reads it.
function frame(id: number): Frame {
  return {
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    roll: false,
    pointCount: id,
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
    sampleRate: 48000,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
  };
}

function setup() {
  const pending: ((timestamp: number) => void)[] = [];
  const drawn: number[] = [];
  let credits = 0;
  const loop = new RenderLoop(
    (callback) => pending.push(callback),
    { draw: (drawnFrame) => drawn.push(drawnFrame.pointCount) },
    () => {
      credits++;
    },
  );
  const tick = () => {
    const callbacks = pending.splice(0);
    for (const callback of callbacks) {
      callback(0);
    }
  };
  return { loop, drawn, tick, credits: () => credits, pending };
}

describe("render loop", () => {
  test("draws engine frames no more often than the minimum interval", () => {
    const pending: ((timestamp: number) => void)[] = [];
    const drawn: number[] = [];
    const loop = new RenderLoop(
      (callback) => pending.push(callback),
      { draw: (drawnFrame) => drawn.push(drawnFrame.pointCount) },
      () => undefined,
      15,
    );
    const tickAt = (timestamp: number) => {
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    };
    loop.submit(frame(1));
    tickAt(100);
    loop.submit(frame(2));
    tickAt(108); // 120 Hz: too soon.
    expect(drawn).toEqual([1]);
    expect(pending).toHaveLength(1);
    tickAt(116);
    expect(drawn).toEqual([1, 2]);
  });

  test.each([60, 75, 100, 120, 144, 165])(
    "draws 60 frames a second at %i Hz",
    (refreshRate) => {
      const pending: ((timestamp: number) => void)[] = [];
      let drawn = 0;
      const loop = new RenderLoop(
        (callback) => pending.push(callback),
        {
          draw: () => {
            drawn++;
          },
        },
        () => {
          loop.submit(frame(drawn)); // The engine always has the next frame ready.
        },
        1000 / 60,
      );
      loop.submit(frame(0));
      for (let tick = 0; tick <= refreshRate * 10; tick++) {
        for (const callback of pending.splice(0)) {
          callback((tick * 1000) / refreshRate);
        }
      }
      expect(drawn / 10).toBeCloseTo(60, 0); // Over 10 seconds.
    },
  );

  // A display's ticks reach the page a little early or late: on a 60 Hz MacBook display their
  // timestamps wander by up to 2.5 ms either way, 15% of a tick. A tick that comes early is still
  // the tick to draw on; skipping it leaves a frame on screen for two ticks, a visible stutter.
  test.each([60, 75, 100, 120, 144, 165])(
    "skips no tick it should draw on at %i Hz when ticks wander by 15% of a tick",
    (refreshRate) => {
      const interval = 1000 / 60;
      const period = 1000 / refreshRate;
      const wander = 0.15 * period;
      const pending: ((timestamp: number) => void)[] = [];
      const drawnAt: number[] = [];
      let now = 0;
      const loop = new RenderLoop(
        (callback) => pending.push(callback),
        {
          draw: () => {
            drawnAt.push(now);
          },
        },
        () => {
          loop.submit(frame(drawnAt.length)); // The engine always has the next frame ready.
        },
        interval,
      );
      loop.submit(frame(0));
      let seed = 1;
      for (let tick = 0; tick <= refreshRate * 10; tick++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        now = tick * period + wander * (2 * (seed / 2147483648) - 1);
        for (const callback of pending.splice(0)) {
          callback(now);
        }
      }
      // The longest the 60 frames/s schedule needs between draws: the ticks one interval
      // spans, each of which may have wandered.
      const longest = Math.ceil(interval / period) * period + 2 * wander;
      const gaps = drawnAt
        .slice(1)
        .map((at, index) => at - (drawnAt[index] ?? 0));
      expect(Math.max(...gaps)).toBeLessThanOrEqual(longest);
      expect(drawnAt.length / 10).toBeCloseTo(60, 0); // Over 10 seconds.
    },
  );

  test("does not catch up with a quick pair of draws after a stall", () => {
    const pending: ((timestamp: number) => void)[] = [];
    const drawn: number[] = [];
    const loop = new RenderLoop(
      (callback) => pending.push(callback),
      { draw: (drawnFrame) => drawn.push(drawnFrame.pointCount) },
      () => undefined,
      1000 / 60,
    );
    const tickAt = (timestamp: number) => {
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    };
    loop.submit(frame(1));
    tickAt(0);
    loop.submit(frame(2));
    tickAt(500); // A stall.
    loop.submit(frame(3));
    tickAt(508); // 120 Hz: the next tick.
    expect(drawn).toEqual([1, 2]);
    tickAt(517);
    expect(drawn).toEqual([1, 2, 3]);
  });

  test("draws on the next tick, then returns the credit", () => {
    const { loop, drawn, tick, credits } = setup();
    loop.submit(frame(1));
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
    tick();
    expect(drawn).toEqual([1]);
    expect(credits()).toBe(1);
  });

  test("draws only the newest of several frames per tick", () => {
    const { loop, drawn, tick, credits, pending } = setup();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(pending).toHaveLength(1);
    tick();
    expect(drawn).toEqual([2]);
    expect(credits()).toBe(1);
  });

  test("does nothing and returns no credit while no frame arrives", () => {
    const { drawn, tick, credits } = setup();
    tick();
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
  });

  test("redraws the last frame on request without earning a credit", () => {
    const { loop, drawn, tick, credits } = setup();
    loop.submit(frame(1));
    tick();
    loop.invalidate();
    tick();
    expect(drawn).toEqual([1, 1]);
    expect(credits()).toBe(1);
  });

  // While the page is hidden, requestAnimationFrame stops: no draws, no credits.
  test("waits for the clock while it is paused", () => {
    const { loop, drawn, credits } = setup();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
  });
});
