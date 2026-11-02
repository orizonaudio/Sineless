// Simulated display ticks for the draw loop's tests: the rates displays tick at, how their
// timestamps wander, ticks the page misses, moves between displays, and engines that answer a
// credit at once, late or a block later.
import { FrameMode } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import { RenderLoop } from "../src/render/render-loop.ts";

// The rates a page's requestAnimationFrame commonly ticks at (Hz): displays from 50 to 360 Hz,
// NTSC's 59.94, rates a little above 60 that drift against a 60-a-second schedule, and 30, where
// Low Power Mode and some hosts throttle the page.
export const commonTickRates = [
  30, 50, 59.94, 60, 60.5, 61, 64, 75, 85, 90, 100, 120, 144, 165, 170, 180,
  240, 360,
] as const;

// Park and Miller's generator: exact in a double, so a seed always gives the same run.
export function randomFrom(seed: number): () => number {
  let state = seed;
  return () => {
    state = (state * 16807) % 2147483647;
    return state / 2147483647;
  };
}

// How far a tick's timestamp lands from its refresh (ms), for a period of `period` ms.
export interface Wander {
  readonly name: string;
  offset(period: number, random: () => number): number;
}

function gaussian(sd: number, name: string): Wander {
  return {
    name,
    offset: (_, random) =>
      sd *
      Math.sqrt(-2 * Math.log(random() || 1e-12)) *
      Math.cos(2 * Math.PI * random()),
  };
}

export const wanders = {
  // Up to 15% of a tick either way, evenly spread.
  tickShare: {
    name: "15% of a tick",
    offset: (period: number, random: () => number) =>
      0.15 * period * (2 * random() - 1),
  },
  // Up to 3 ms either way, the most render-loop.ts records for a 60 Hz MacBook.
  threeMilliseconds: {
    name: "3 ms",
    offset: (_: number, random: () => number) => 3 * (2 * random() - 1),
  },
  // What a page's ticks showed in Live on a 100 Hz display (sd 1.23 ms about a fitted grid).
  recorded: gaussian(1.23, "the recorded 1.23 ms"),
  // A little more than recorded.
  gaussian: gaussian(1.5, "1.5 ms (Gaussian)"),
} satisfies Record<string, Wander>;

// A frame told apart from the others by its point count, `id`; the loop never reads it.
export function frame(id: number): Frame {
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

// The loop as the app builds it.
export function pageLoop(
  schedule: ConstructorParameters<typeof RenderLoop>[0],
  renderer: ConstructorParameters<typeof RenderLoop>[1],
  returnCredit: () => void,
): RenderLoop {
  return new RenderLoop(schedule, renderer, returnCredit);
}

export interface Segment {
  readonly rate: number; // Hz
  readonly seconds: number;
}

export interface Draw {
  readonly refresh: number; // When the refresh it was drawn on happened (ms), without wander.
  readonly period: number; // That display's period (ms).
}

export interface DriveOptions {
  readonly segments: readonly Segment[];
  readonly wander?: Wander;
  readonly seed?: number;
  // The share of refreshes the page gets no callback for.
  readonly skipped?: number;
  // When the frame asked for by the credit returned at `creditAt` (ms, without wander) arrives:
  // undefined for at once.
  readonly arrival?: (creditAt: number, credits: number) => number | undefined;
}

// Ticks the display through `segments`, with the engine's next frame arriving when `arrival`
// says, and returns each engine frame's draw.
export function drive(options: DriveOptions): {
  draws: Draw[];
  loop: RenderLoop;
} {
  const random = randomFrom(options.seed ?? 7);
  const pending: ((timestamp: number) => void)[] = [];
  const draws: Draw[] = [];
  let refresh = 0;
  let period = 0;
  let credits = 0;
  let due: number | undefined;
  const loop = pageLoop(
    (callback) => pending.push(callback),
    {
      draw: () => {
        draws.push({ refresh, period });
      },
    },
    () => {
      credits++;
      const at = options.arrival?.(refresh, credits);
      if (at === undefined) {
        loop.submit(frame(credits));
      } else {
        due = at;
      }
    },
  );
  loop.submit(frame(0));
  let start = 0;
  let last = -Infinity;
  for (const segment of options.segments) {
    period = 1000 / segment.rate;
    const count = Math.round(segment.rate * segment.seconds);
    for (let index = 0; index < count; index++) {
      refresh = start + index * period;
      if (due !== undefined && due <= refresh) {
        due = undefined;
        loop.submit(frame(credits));
      }
      const offset = options.wander?.offset(period, random) ?? 0;
      if (options.skipped !== undefined && random() < options.skipped) {
        continue;
      }
      // Timestamps never run backwards, however much they wander.
      const timestamp = Math.max(refresh + offset, last + 0.01);
      last = timestamp;
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    }
    start += count * period;
  }
  return { draws, loop };
}

// The whole refreshes between successive draws from `from` ms on, on the display each later draw
// was on.
export function heldTicks(draws: readonly Draw[], from = 0): number[] {
  const held: number[] = [];
  for (let index = 1; index < draws.length; index++) {
    const draw = draws[index];
    const before = draws[index - 1];
    if (draw === undefined || before === undefined || before.refresh < from) {
      continue;
    }
    held.push(Math.round((draw.refresh - before.refresh) / draw.period));
  }
  return held;
}
