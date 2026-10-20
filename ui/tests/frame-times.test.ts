import { afterEach, describe, expect, test, vi } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import {
  FrameTimes,
  frameMilliseconds,
  frameTimes,
  lateFactor,
  summarise,
} from "../src/frame-times.ts";
import { RenderLoop } from "../src/render/render-loop.ts";

// Times `every` ms apart from 0, `count` of them.
const steady = (every: number, count: number): number[] =>
  Array.from({ length: count }, (_, index) => index * every);

describe("summarise", () => {
  test("finds nothing late in a steady display", () => {
    const summary = summarise(steady(1000 / 60, 120));
    expect(summary.count).toBe(119);
    expect(summary.median).toBeCloseTo(1000 / 60);
    expect(summary.lateShare).toBe(0);
    expect(summary.worstSecondLate).toBe(0);
  });

  test("measures nothing, not a perfect run, when no frame was drawn", () => {
    for (const times of [[], [100]]) {
      const summary = summarise(times, 1000 / 60);
      expect(summary.count).toBe(0);
      for (const measure of [
        summary.median,
        summary.p95,
        summary.p99,
        summary.max,
        summary.lateShare,
        summary.worstSecondLate,
      ]) {
        expect(measure).toBeNaN();
      }
    }
  });

  test("counts a skipped tick as late, and shows it in the slowest percentiles", () => {
    const times = steady(10, 101);
    times.splice(50, 1); // One tick missing: a 20 ms interval among 10 ms ones.
    const summary = summarise(times);
    expect(summary.lateShare).toBeCloseTo(1 / 99);
    expect(summary.worstSecondLate).toBe(1);
    expect(summary.max).toBe(20);
    expect(summary.median).toBe(10);
  });

  test("shows occasional stutters in the 95th and 99th percentiles, not the median", () => {
    // 3 intervals in 100 three frames long, spread out.
    const times = [0];
    for (let index = 1; index <= 100; index++) {
      times.push((times.at(-1) ?? 0) + (index % 33 === 0 ? 50 : 16));
    }
    const summary = summarise(times, frameMilliseconds);
    expect(summary.median).toBe(16);
    expect(summary.p99).toBe(50);
    expect(summary.p95).toBe(16);
    expect(summary.lateShare).toBeCloseTo(3 / 100);
    // At 6 in 100, the 95th percentile shows them too.
    const more = [0];
    for (let index = 1; index <= 100; index++) {
      more.push((more.at(-1) ?? 0) + (index % 16 === 0 ? 50 : 16));
    }
    expect(summarise(more, frameMilliseconds).p95).toBe(50);
  });

  test("judges frames against their pace, so a display's uneven ticks are not late", () => {
    // 60 frames a second drawn on a 144 Hz display's ticks: 2 or 3 ticks apart.
    const tick = 1000 / 144;
    const times = [0];
    for (let index = 1; index < 120; index++) {
      times.push((times.at(-1) ?? 0) + (index % 2 === 0 ? 2 : 3) * tick);
    }
    expect(summarise(times, frameMilliseconds).lateShare).toBe(0);
    // A frame one whole frame late is.
    times.push((times.at(-1) ?? 0) + lateFactor * frameMilliseconds + tick);
    expect(summarise(times, frameMilliseconds).lateShare).toBeGreaterThan(0);
  });

  test("gives the most late intervals that end within any one second", () => {
    // Three late frames close together, then one more two seconds on.
    const times = [0, 16, 60, 76, 120, 136, 180, 2180, 2196];
    const summary = summarise(times, 16);
    expect(summary.worstSecondLate).toBe(3);
    expect(summary.lateShare).toBeCloseTo(4 / 8);
  });

  test("has no intervals, and no share, without two events", () => {
    expect(summarise([]).count).toBe(0);
    expect(summarise([5]).lateShare).toBeNaN();
  });
});

describe("FrameTimes", () => {
  function recorder(capacity?: number) {
    const pending: ((timestamp: number) => void)[] = [];
    const times = new FrameTimes(
      (callback) => pending.push(callback),
      capacity,
    );
    const tick = (timestamp: number) => {
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    };
    return { times, tick, pending };
  }

  test("records nothing until started, then every tick and every frame drawn", () => {
    const { times, tick } = recorder();
    times.drawn(1);
    tick(1);
    expect(times.summary().frames.count).toBe(0);
    times.start();
    for (const at of [10, 20, 30]) {
      tick(at);
      times.drawn(at);
    }
    expect(times.summary().ticks.count).toBe(2);
    expect(times.summary().frames.median).toBe(10);
  });

  test("stops its loop when stopped, and starting again starts afresh with one loop", () => {
    const { times, tick, pending } = recorder();
    times.start();
    tick(0);
    times.stop();
    tick(10);
    expect(pending).toHaveLength(0);
    times.start();
    times.start();
    tick(100);
    tick(110);
    expect(pending).toHaveLength(1);
    expect(times.summary().ticks.count).toBe(1);
  });

  test("judges drawn frames against 60 a second, not their own median", () => {
    // Mostly 10 ms apart, with every fourth 23 ms after the one before: none a whole frame late
    // at 60 a second, though all would be against their own 10 ms median.
    const { times, tick } = recorder();
    times.start();
    let at = 0;
    for (let index = 0; index < 60; index++) {
      at += index % 4 === 3 ? 23 : 10;
      tick(at);
      times.drawn(at);
    }
    expect(times.summary().frames.median).toBe(10);
    expect(times.summary().frames.lateShare).toBe(0);
  });

  test("keeps the newest times when it runs longer than it holds", () => {
    const { times, tick } = recorder(4);
    times.start();
    for (const at of [0, 10, 20, 30, 40, 100]) {
      tick(at);
    }
    // 20, 30, 40 and 100 kept: the 60 ms gap is the newest.
    expect(times.summary().ticks.max).toBe(60);
    expect(times.summary().ticks.count).toBe(3);
  });
});

describe("the page's draw loop", () => {
  afterEach(() => {
    frameTimes.stop();
    vi.unstubAllGlobals();
  });

  test("measures the display's ticks through the page's own scheduler", () => {
    const pending: ((timestamp: number) => void)[] = [];
    vi.stubGlobal(
      "requestAnimationFrame",
      (callback: (timestamp: number) => void) => {
        pending.push(callback);
        return pending.length;
      },
    );
    frameTimes.start();
    for (const at of [0, 10, 20, 30]) {
      for (const callback of pending.splice(0)) {
        callback(at);
      }
    }
    const ticks = frameTimes.summary().ticks;
    expect(ticks.count).toBe(3);
    expect(ticks.median).toBe(10);
  });

  test("records the engine frames it draws, not its redraws", () => {
    vi.stubGlobal("requestAnimationFrame", () => 0); // The recorder's own ticks are not wanted.
    const pending: ((timestamp: number) => void)[] = [];
    const loop = new RenderLoop(
      (callback) => pending.push(callback),
      { draw: () => undefined },
      () => undefined,
    );
    const tick = (timestamp: number) => {
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    };
    frameTimes.start();
    loop.submit(frame());
    tick(100);
    loop.invalidate();
    tick(110);
    loop.submit(frame());
    tick(120);
    const frames = frameTimes.summary().frames;
    expect(frames.count).toBe(1);
    expect(frames.median).toBe(20);
  });
});

function frame(): Frame {
  return {
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    roll: false,
    pointCount: 1,
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
