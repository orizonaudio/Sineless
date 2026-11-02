import { describe, expect, test } from "vitest";

import {
  fullRateUpToHz,
  lineHysteresis,
  ticksPerFrame,
} from "../src/render/render-loop.ts";
import {
  commonTickRates,
  drive,
  frame,
  heldTicks,
  pageLoop,
  wanders,
  type Wander,
} from "./display-ticks.ts";

// A frame stays on screen for the ticks until the next one is drawn. Uneven holds (2, 1, 2 on a
// 100 Hz display, under the old 60-a-second schedule) make a playhead that moves by the time
// between frames judder; each frame should stay for the same n ticks.
describe("render loop cadence", () => {
  const settled = 1000; // ms: by then the loop has measured the display.

  // A tick rate at or below 120 Hz holds every frame for exactly n ticks under any wander a page
  // sees. Above it, a gap that wanders past 1.75 ticks reads as skipped callbacks, which the loop
  // recovers at the cost of these rare misreadings, and large wander mismeasures the rate: those
  // rows bound the share of uneven holds measured for this loop.
  const unevenAllowed = (rate: number, wander: Wander): number => {
    if (rate <= 120 || wander === wanders.tickShare) {
      return 0;
    }
    if (wander === wanders.recorded) {
      return rate <= 170 ? 0.01 : rate <= 180 ? 0.03 : 0.4;
    }
    return rate <= 144 ? 0.02 : rate <= 170 ? 0.05 : rate <= 180 ? 0.1 : 0.6;
  };

  const rows = commonTickRates.flatMap((rate) =>
    Object.values(wanders).map((wander) => ({
      rate,
      wander,
      name: wander.name,
    })),
  );
  test.each(rows)(
    "holds each frame for the same number of ticks at $rate Hz, ticks wandering by $name",
    ({ rate, wander }) => {
      const n = ticksPerFrame(rate);
      const { draws } = drive({ segments: [{ rate, seconds: 10 }], wander });
      const held = heldTicks(draws, settled);
      const uneven = held.filter((ticks) => ticks !== n).length / held.length;
      expect(uneven).toBeLessThanOrEqual(unevenAllowed(rate, wander));
      if (unevenAllowed(rate, wander) === 0) {
        expect(held.length / 9).toBeCloseTo(rate / n, -1);
      }
    },
  );

  // A callback the page misses is a refresh it cannot draw on; the frame waiting is drawn on the
  // next one, and the next frame is again n ticks after it.
  test.each(commonTickRates.map((rate) => ({ rate })))(
    "holds a frame at most one tick longer for a missed callback at $rate Hz",
    ({ rate }) => {
      const n = ticksPerFrame(rate);
      const { draws } = drive({
        segments: [{ rate, seconds: 10 }],
        wander: wanders.tickShare,
        skipped: 0.01,
      });
      for (const ticks of heldTicks(draws, settled)) {
        expect([n, n + 1]).toContain(ticks);
      }
    },
  );

  // A callback missed inside a hold (after a long draw, say) is a gap of two ticks, counted as
  // two: the next frame is still drawn n ticks after the last, not held a tick longer.
  test.each(
    commonTickRates
      .filter((rate) => ticksPerFrame(rate) > 1)
      .map((rate) => ({ rate })),
  )(
    "draws n ticks after the last frame when a callback in between is missed at $rate Hz",
    ({ rate }) => {
      const n = ticksPerFrame(rate);
      const period = 1000 / rate;
      const pending: ((timestamp: number) => void)[] = [];
      const drawnAt: number[] = [];
      let index = 0;
      const loop = pageLoop(
        (callback) => pending.push(callback),
        {
          draw: () => {
            drawnAt.push(index);
          },
        },
        () => {
          loop.submit(frame(drawnAt.length));
        },
      );
      const refresh = (missed = false) => {
        index++;
        if (!missed) {
          for (const callback of pending.splice(0)) {
            callback(index * period);
          }
        }
      };
      loop.submit(frame(0));
      while (index < 2 * rate || drawnAt.at(-1) !== index) {
        refresh(); // Measured, and just drawn.
      }
      const last = index;
      refresh(true); // The callback right after the draw is missed.
      while (drawnAt.at(-1) === last) {
        refresh();
      }
      expect(drawnAt.at(-1)).toBe(last + n);
    },
  );

  // Many missed callbacks double some gaps; the rate is still measured from the others.
  test("keeps n at 100 Hz when one callback in ten is missed", () => {
    const { draws } = drive({
      segments: [{ rate: 100, seconds: 10 }],
      wander: wanders.tickShare,
      skipped: 0.1,
    });
    expect(heldTicks(draws, settled)).not.toContain(1);
  });

  test("draws every tick just below the line when callbacks are missed", () => {
    const rate = 0.99 * ticksPerFrameLine();
    const { draws } = drive({
      segments: [{ rate, seconds: 10 }],
      wander: wanders.tickShare,
      skipped: 0.05,
    });
    const held = heldTicks(draws, settled);
    expect(
      held.filter((ticks) => ticks === 1).length / held.length,
    ).toBeGreaterThan(0.85);
  });

  // Gaps of 50 ms or more are stalls, not ticks: a run of them leaves the measured rate alone,
  // while a run of slightly shorter ones is a slow display, measured as one.
  test.each([
    { gap: 49, n: 1 },
    { gap: 51, n: 2 },
  ])("measures a run of $gap ms gaps at 100 Hz as n = $n", ({ gap, n }) => {
    const pending: ((timestamp: number) => void)[] = [];
    const drawnAt: number[] = [];
    let now = 0;
    const loop = pageLoop(
      (callback) => pending.push(callback),
      {
        draw: () => {
          drawnAt.push(now);
        },
      },
      () => {
        loop.submit(frame(drawnAt.length));
      },
    );
    const tickAt = (at: number) => {
      now = at;
      for (const callback of pending.splice(0)) {
        callback(at);
      }
    };
    loop.submit(frame(0));
    for (let tick = 0; tick < 100; tick++) {
      tickAt(tick * 10);
    }
    for (let tick = 1; tick <= 10; tick++) {
      tickAt(990 + tick * gap);
    }
    const from = now;
    for (let tick = 1; tick <= 4; tick++) {
      tickAt(from + tick * 10);
    }
    // Right after the run, before the 10 ms ticks move the measure back.
    const after = drawnAt.filter((at) => at > from);
    expect(after.length).toBe(n === 1 ? 4 : 2);
  });
});

// The 95 Hz line, from the loop's own rule.
function ticksPerFrameLine(): number {
  let rate = 50;
  while (ticksPerFrame(rate) === 1) {
    rate += 0.01;
  }
  return rate;
}

describe("render loop on a display change", () => {
  // A display drifting either side of a line, by less than the hysteresis, keeps its n: each
  // frame stays for the same ticks rather than switching between n and n + 1.
  test.each([1, 2, 3].map((below) => ({ below })))(
    "keeps n while the tick rate drifts across the line above $below frames' worth",
    ({ below }) => {
      const line = fullRateUpToHz * below;
      const low = line * (1 - lineHysteresis / 2);
      const high = line * (1 + lineHysteresis / 2);
      const n = ticksPerFrame(low);
      const { draws } = drive({
        segments: [low, high, low, high].map((rate) => ({ rate, seconds: 2 })),
        wander: wanders.tickShare,
      });
      for (const ticks of heldTicks(draws, 1000)) {
        expect(ticks).toBe(n);
      }
    },
  );

  // Gaps after `move` ms: the first spans the move and is left out.
  const heldAfter = (
    segments: { rate: number; seconds: number }[],
    move: number,
  ) => {
    const { draws } = drive({ segments, wander: wanders.tickShare });
    const after = draws.filter((draw) => draw.refresh >= move);
    return heldTicks(after).slice(1);
  };

  // Moved to a display of another rate, the loop measures the new one's ticks and holds frames
  // for its n within half a second.
  test.each([
    [60, 120],
    [120, 60],
    [100, 60],
    [60, 100],
    [120, 30],
    [90, 40],
  ])("follows the display from %i Hz to %i Hz", (from, to) => {
    const { draws } = drive({
      segments: [
        { rate: from, seconds: 5 },
        { rate: to, seconds: 5 },
      ],
      wander: wanders.tickShare,
    });
    const settledAfterMove = draws.filter((draw) => draw.refresh >= 5500);
    for (const ticks of heldTicks(settledAfterMove)) {
      expect(ticks).toBe(ticksPerFrame(to));
    }
  });

  // Rates under a quarter apart on either side of the line: the measure follows more slowly, and
  // n changes once, with no return.
  test.each([
    [90, 100],
    [100, 90],
    [75, 100],
  ])("changes n once, within 1.5 s, from %i Hz to %i Hz", (from, to) => {
    const held = heldAfter(
      [
        { rate: from, seconds: 5 },
        { rate: to, seconds: 5 },
      ],
      5000,
    );
    const before = ticksPerFrame(from);
    const after = ticksPerFrame(to);
    const change = held.findIndex((ticks) => ticks !== before);
    expect(change).toBeGreaterThanOrEqual(0);
    expect(held.slice(change).every((ticks) => ticks === after)).toBe(true);
    // Within 1.5 s: the draws up to the change span no more than that.
    const span = held
      .slice(0, change)
      .reduce((sum, ticks) => sum + (ticks * 1000) / to, 0);
    expect(span).toBeLessThanOrEqual(1500);
  });
});

describe("render loop with late frames", () => {
  // One frame arrives half an interval after its tick; the engine is otherwise always ready. It is
  // drawn on the first tick after it arrives, and the next frame n ticks after that: one longer
  // hold, not a long one and a short one.
  test.each([100, 120, 144, 165, 240])(
    "keeps n ticks after a late frame at %i Hz",
    (rate) => {
      const n = ticksPerFrame(rate);
      const period = 1000 / rate;
      const { draws } = drive({
        segments: [{ rate, seconds: 5 }],
        wander: wanders.tickShare,
        arrival: (creditAt, credits) =>
          credits === 200 ? creditAt + (n + 0.5) * period : undefined,
      });
      // It arrives n + 0.5 ticks after its credit, so the first tick after it is n + 1.
      expect(heldTicks(draws, 1000).filter((ticks) => ticks !== n)).toEqual([
        n + 1,
      ]);
    },
  );

  // Each frame arrives at the first block boundary after its credit, as the engine answers once
  // new audio arrives (editor/src/FrameFlow.cpp).
  const blockRows = [44100, 48000, 96000, 192000, 384000].flatMap(
    (sampleRate) =>
      [64, 256, 512, 1024, 4096].flatMap((samples) =>
        [60, 100, 144].map((rate) => ({ sampleRate, samples, rate })),
      ),
  );
  test.each(blockRows)(
    "never draws two frames less than n ticks apart at $rate Hz with $samples-sample blocks at $sampleRate Hz",
    ({ sampleRate, samples, rate }) => {
      const n = ticksPerFrame(rate);
      const period = 1000 / rate;
      const block = (samples / sampleRate) * 1000;
      const { draws } = drive({
        segments: [{ rate, seconds: 10 }],
        arrival: (creditAt) => (Math.floor(creditAt / block) + 1) * block,
      });
      const held = heldTicks(draws, 1000);
      expect(Math.min(...held)).toBeGreaterThanOrEqual(n);
      const perSecond = held.length / 9;
      expect(perSecond).toBeLessThanOrEqual(
        Math.min(rate / n, 1000 / block) + 1,
      );
      expect(perSecond).toBeGreaterThanOrEqual(
        1000 / (Math.max(n * period, block) + period) - 1,
      );
    },
  );
});

// At n = 2 on a 100 Hz display, after a second of always-ready frames has measured it: then the
// engine sends nothing unless a test submits.
function measuredAt100Hz(throwOn?: number) {
  const pending: ((timestamp: number) => void)[] = [];
  const drawn: number[] = [];
  let credits = 0;
  let ready = true;
  let now = 0;
  const loop = pageLoop(
    (callback) => pending.push(callback),
    {
      draw: (drawnFrame) => {
        if (drawnFrame.pointCount === throwOn) {
          throw new Error("draw failed");
        }
        drawn.push(drawnFrame.pointCount);
      },
    },
    () => {
      credits++;
      if (ready) {
        loop.submit(frame(1000 + credits));
      }
    },
  );
  const tick = (step = 10) => {
    now += step;
    for (const callback of pending.splice(0)) {
      callback(now);
    }
  };
  loop.submit(frame(1000));
  for (let index = 0; index < 100; index++) {
    tick();
  }
  ready = false;
  // Out of frames: tick until the loop asks for no more ticks.
  for (let index = 0; index < 10 && pending.length > 0; index++) {
    tick();
  }
  drawn.length = 0;
  return {
    loop,
    drawn,
    pending,
    tick,
    credits: () => credits,
  };
}

describe("render loop at two ticks a frame", () => {
  test("asks for no ticks once frames stop", () => {
    const { loop, pending, tick } = measuredAt100Hz();
    expect(pending).toHaveLength(0);
    loop.submit(frame(1));
    tick();
    // Drawn; it asks for the ticks up to the next frame's turn, then stops.
    for (let index = 0; index < 2; index++) {
      tick();
    }
    expect(pending).toHaveLength(0);
  });

  test("asks for no ticks once frames stop, after a draw that throws", () => {
    const { loop, pending, tick, credits } = measuredAt100Hz(1);
    loop.submit(frame(1));
    expect(() => {
      tick();
    }).toThrow("draw failed");
    const after = credits();
    for (let index = 0; index < 2; index++) {
      tick();
    }
    expect(pending).toHaveLength(0);
    expect(credits()).toBe(after);
  });

  test("draws on the next tick once ticks have stopped, then returns the credit", () => {
    const { loop, drawn, tick, credits } = measuredAt100Hz();
    const before = credits();
    loop.submit(frame(1));
    expect(drawn).toEqual([]);
    tick();
    expect(drawn).toEqual([1]);
    expect(credits()).toBe(before + 1);
  });

  test("returns the credit and keeps drawing after a draw that throws", () => {
    const { loop, drawn, tick, credits } = measuredAt100Hz(1);
    const before = credits();
    loop.submit(frame(1));
    expect(() => {
      tick();
    }).toThrow("draw failed");
    expect(credits()).toBe(before + 1);
    loop.submit(frame(2));
    tick();
    tick();
    expect(drawn).toEqual([2]);
    expect(credits()).toBe(before + 2);
  });

  test("does not catch up with a quick pair of draws after a stall", () => {
    const { loop, drawn, tick } = measuredAt100Hz();
    loop.submit(frame(1));
    tick();
    loop.submit(frame(2));
    tick(500); // A stall.
    loop.submit(frame(3));
    tick(); // The next tick: too soon for the next frame.
    expect(drawn).toEqual([1, 2]);
    tick();
    expect(drawn).toEqual([1, 2, 3]);
  });

  test("draws only the newest of several frames", () => {
    const { loop, drawn, tick, credits, pending } = measuredAt100Hz();
    const before = credits();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(pending).toHaveLength(1);
    tick();
    expect(drawn).toEqual([2]);
    expect(credits()).toBe(before + 1);
  });

  test("does nothing and returns no credit while no frame arrives", () => {
    const { drawn, tick, credits } = measuredAt100Hz();
    const before = credits();
    tick();
    expect(drawn).toEqual([]);
    expect(credits()).toBe(before);
  });

  test("redraws the last frame on request without earning a credit", () => {
    const { loop, drawn, tick, credits } = measuredAt100Hz();
    loop.submit(frame(1));
    tick();
    tick();
    tick();
    const before = credits();
    loop.invalidate();
    tick();
    expect(drawn).toEqual([1, 1]);
    expect(credits()).toBe(before);
  });

  // A redraw asked for while a frame waits for its tick is served by that frame's draw: the older
  // frame is not drawn in between.
  test("serves a redraw with the frame waiting for its tick", () => {
    const { loop, drawn, tick } = measuredAt100Hz();
    loop.submit(frame(1));
    tick();
    loop.submit(frame(2));
    loop.invalidate();
    tick(); // One tick since the draw: frame 2 waits for the second.
    expect(drawn).toEqual([1]);
    tick();
    expect(drawn).toEqual([1, 2]);
    tick();
    tick();
    expect(drawn).toEqual([1, 2]);
  });

  // While the page is hidden, requestAnimationFrame stops: no draws, no credits.
  test("waits for the clock while it is paused", () => {
    const { loop, drawn, credits } = measuredAt100Hz();
    const before = credits();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(drawn).toEqual([]);
    expect(credits()).toBe(before);
  });
});
