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
    freeRunning: false,
    pointCount: id,
    startFraction: 0,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    partial: false,
    noTempo: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    panReach: 0,
    invalidSamples: 0,
    invalidLanes: 0,
    sampleRate: 48000,
    spanSeconds: 0,
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
        seed = (seed * 16807) % 2147483647; // Park and Miller's: exact in a double.
        now = tick * period + wander * (2 * (seed / 2147483647) - 1);
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

  // The engine answers a credit with the next frame only once new audio arrives: with 512-sample
  // blocks at 48 kHz, up to 10.7 ms later, past the next tick of a fast display. The loop still
  // measures the display's ticks, not the gaps between frames, and draws 60 evenly spaced frames
  // a second.
  test.each([120, 144, 165, 240])(
    "draws 60 even frames a second at %i Hz when each frame comes a block after the credit",
    (refreshRate) => {
      const interval = 1000 / 60;
      const period = 1000 / refreshRate;
      const block = (512 / 48000) * 1000;
      const pending: ((timestamp: number) => void)[] = [];
      const drawnAt: number[] = [];
      let now = 0;
      let due: number | undefined;
      const loop = new RenderLoop(
        (callback) => pending.push(callback),
        {
          draw: () => {
            drawnAt.push(now);
          },
        },
        () => {
          due = (Math.floor(now / block) + 1) * block;
        },
        interval,
      );
      loop.submit(frame(0));
      for (let tick = 0; tick <= refreshRate * 10; tick++) {
        now = tick * period;
        if (due !== undefined && due <= now) {
          due = undefined;
          loop.submit(frame(drawnAt.length));
        }
        for (const callback of pending.splice(0)) {
          callback(now);
        }
      }
      const settled = drawnAt.filter((at) => at >= 1000);
      expect(settled.length / 9).toBeCloseTo(60, 0);
      const gaps = settled
        .slice(1)
        .map((at, index) => at - (settled[index] ?? 0));
      expect(Math.max(...gaps)).toBeLessThanOrEqual(
        Math.ceil(interval / period) * period + 1e-9,
      );
    },
  );

  // Ticks at `rates` (Hz) in turn, `seconds` of each, wandering by 15% of a tick, with the
  // engine's next frame always ready: when each frame was drawn, and how many ticks there were.
  function drawsAt(rates: readonly number[], seconds: number) {
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
        loop.submit(frame(drawnAt.length));
      },
      1000 / 60,
    );
    loop.submit(frame(0));
    let seed = 7;
    let start = 0;
    let ticks = 0;
    for (const rate of rates) {
      const period = 1000 / rate;
      for (let tick = 0; tick < rate * seconds; tick++) {
        seed = (seed * 16807) % 2147483647; // Park and Miller's: exact in a double.
        now =
          start + tick * period + 0.15 * period * (2 * (seed / 2147483647) - 1);
        ticks++;
        for (const callback of pending.splice(0)) {
          callback(now);
        }
      }
      start += seconds * 1000;
    }
    return { drawnAt, ticks };
  }

  // A display at or near 60 Hz is not paced: its ticks drift from an exact 60 a second, and a
  // fixed schedule would sooner or later find one early and skip it, a stutter. (The slowest
  // displays paced, at 75 Hz, are covered above.)
  test.each([59.94, 60, 60.5, 61])(
    "draws on every tick of a %f Hz display",
    (refreshRate) => {
      const { drawnAt, ticks } = drawsAt([refreshRate], 30);
      expect(drawnAt).toHaveLength(ticks);
    },
  );

  // Moved to a display of another rate, the loop measures the new one's ticks and paces for
  // it within a second: 60 frames a second at 120 Hz, and every tick again back at 60 Hz.
  // From a display it does not pace, including one a little faster than 60 Hz, whose ticks
  // would otherwise leave a 60 frames/s schedule behind.
  test.each([60, 64])(
    "follows the display when the window moves from %i Hz to 120 Hz and back",
    (first) => {
      const { drawnAt } = drawsAt([first, 120, first], 5);
      const inSecond = (second: number) =>
        drawnAt.filter((at) => at >= second * 1000 && at < (second + 1) * 1000)
          .length;
      expect(inSecond(4)).toBeGreaterThanOrEqual(first - 1); // Every tick.
      expect(inSecond(9)).toBeCloseTo(60, -1); // 120 Hz, settled.
      expect(inSecond(9)).toBeLessThanOrEqual(61);
      expect(inSecond(14)).toBeGreaterThanOrEqual(first - 1); // Every tick again.
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

  test("returns the credit and keeps drawing after a draw that throws", () => {
    const pending: ((timestamp: number) => void)[] = [];
    const drawn: number[] = [];
    let credits = 0;
    const loop = new RenderLoop(
      (callback) => pending.push(callback),
      {
        draw: (drawnFrame) => {
          if (drawnFrame.pointCount === 1) {
            throw new Error("draw failed");
          }
          drawn.push(drawnFrame.pointCount);
        },
      },
      () => {
        credits++;
      },
    );
    const tick = () => {
      for (const callback of pending.splice(0)) {
        callback(0);
      }
    };
    loop.submit(frame(1));
    expect(tick).toThrow("draw failed");
    expect(credits).toBe(1);
    loop.submit(frame(2));
    tick();
    expect(drawn).toEqual([2]);
    expect(credits).toBe(2);
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
