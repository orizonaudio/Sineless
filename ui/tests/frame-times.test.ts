import { afterEach, describe, expect, test, vi } from "vitest";

import {
  type FrameAim,
  FrameTimes,
  frameTimes,
  summarise,
  summariseFrames,
} from "../src/frame-times.ts";
import { ticksPerFrame } from "../src/render/render-loop.ts";
import { drive, frame, heldTicks, pageLoop } from "./display-ticks.ts";

const sixtyHz = 1000 / 60;

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
    const summary = summarise(times, sixtyHz);
    expect(summary.median).toBe(16);
    expect(summary.p99).toBe(50);
    expect(summary.p95).toBe(16);
    expect(summary.lateShare).toBeCloseTo(3 / 100);
    // At 6 in 100, the 95th percentile shows them too.
    const more = [0];
    for (let index = 1; index <= 100; index++) {
      more.push((more.at(-1) ?? 0) + (index % 16 === 0 ? 50 : 16));
    }
    expect(summarise(more, sixtyHz).p95).toBe(50);
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

describe("summariseFrames", () => {
  // Frames n ticks apart on a display ticking `rate` times a second, with each draw's aim.
  const aimAt = (rate: number): FrameAim => {
    const tick = 1000 / rate;
    return { interval: ticksPerFrame(rate) * tick, tick };
  };

  // Where the aim's limit differs from a fixed 60-a-second one (25 ms): n = 2 at 120 and 144 Hz.
  test.each([120, 144])(
    "counts a frame held a tick too long as late at %i Hz, and wander of under half a tick as not",
    (rate) => {
      const aim = aimAt(rate);
      const times = [0];
      const step = (interval: number) => {
        times.push((times.at(-1) ?? 0) + interval);
      };
      for (let index = 0; index < 20; index++) {
        step(aim.interval);
      }
      step(aim.interval + aim.tick); // Held a tick too long.
      step(aim.interval + 0.4 * aim.tick); // Wander.
      const summary = summariseFrames(
        times,
        times.map(() => aim),
      );
      expect(summary.count).toBe(22);
      expect(summary.lateShare).toBeCloseTo(1 / 22);
    },
  );

  // The benchmark's "worst second": a run with a late frame now and then has one late frame in
  // its worst second, however long it runs; a cluster of them shows as that many.
  test("gives the most late frames that end within any one second", () => {
    const aim: FrameAim = { interval: 20, tick: 10 }; // Late past 25 ms.
    const times = [0];
    const step = (interval: number) => {
      times.push((times.at(-1) ?? 0) + interval);
    };
    step(30); // Late, first of the run.
    for (let index = 0; index < 100; index++) {
      step(aim.interval);
    }
    step(30); // Late, two seconds after the first.
    for (let index = 0; index < 100; index++) {
      step(aim.interval);
    }
    step(30); // Three late together.
    step(30);
    step(30);
    const summary = summariseFrames(
      times,
      times.map(() => aim),
    );
    expect(summary.worstSecondLate).toBe(3);
    const sparse = summariseFrames(
      times.slice(0, 203),
      times.map(() => aim),
    );
    expect(sparse.worstSecondLate).toBe(1);
  });

  test("judges each interval by the aim of the draw before it", () => {
    // 120 Hz (n = 2, limit 20.8 ms), then 60 Hz (n = 1, limit 25 ms): a 22 ms interval is late
    // after a draw aimed at 120 Hz, and on time after one aimed at 60 Hz.
    const times = [0, 16.7, 38.7, 55.4, 72.1, 94.1];
    const aims = [
      aimAt(120),
      aimAt(120),
      aimAt(120),
      aimAt(60),
      aimAt(60),
      aimAt(60),
    ];
    const summary = summariseFrames(times, aims);
    expect(summary.count).toBe(5);
    expect(summary.lateShare).toBeCloseTo(1 / 5);
  });

  test("leaves out intervals after a draw the loop had no aim for", () => {
    const aim = aimAt(60);
    const times = [0, 40, 80, 96.7, 113.3];
    const summary = summariseFrames(times, [
      undefined,
      undefined,
      aim,
      aim,
      aim,
    ]);
    expect(summary.count).toBe(2);
    expect(summary.lateShare).toBe(0);
  });

  test("at one tick a frame, counts an interval late beyond one and a half ticks, as before", () => {
    const aim = aimAt(60);
    expect(summariseFrames([0, 24.9], [aim, aim]).lateShare).toBe(0);
    expect(summariseFrames([0, 25.1], [aim, aim]).lateShare).toBe(1);
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
      times.drawn(at, { interval: 10, tick: 10 });
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
    let ready = true;
    const loop = pageLoop(
      (callback) => pending.push(callback),
      { draw: () => undefined },
      () => {
        if (ready) {
          loop.submit(frame(1));
        }
      },
    );
    let now = 0;
    const tick = () => {
      now += 10;
      for (const callback of pending.splice(0)) {
        callback(now);
      }
    };
    loop.submit(frame(0));
    for (let index = 0; index < 50; index++) {
      tick(); // 100 Hz: measured, two ticks a frame.
    }
    ready = false;
    for (let index = 0; index < 4; index++) {
      tick();
    }
    frameTimes.start();
    loop.submit(frame(1));
    tick();
    loop.invalidate();
    tick();
    tick();
    loop.submit(frame(1));
    tick();
    const frames = frameTimes.summary().frames;
    expect(frames.count).toBe(1);
    expect(frames.median).toBe(30);
  });

  // The loop passes what it aimed for: at 120 Hz, two ticks. A frame that comes a tick late is
  // the one late interval; the draws before the loop measured the display are left out.
  test("counts the one frame drawn a tick late at 120 Hz", () => {
    vi.stubGlobal("requestAnimationFrame", () => 0);
    frameTimes.start();
    const period = 1000 / 120;
    const { draws } = drive({
      segments: [{ rate: 120, seconds: 3 }],
      arrival: (creditAt, credits) =>
        credits === 150 ? creditAt + 2.5 * period : undefined,
    });
    const frames = frameTimes.summary().frames;
    expect(heldTicks(draws, 1000).filter((ticks) => ticks === 3)).toHaveLength(
      1,
    );
    expect(frames.count).toBeLessThan(draws.length - 1);
    expect(frames.lateShare * frames.count).toBeCloseTo(1);
  });
});
