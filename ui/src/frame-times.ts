// Frame timing for the editor's benchmark (editor/bench/PerfBench.mm), which starts it through
// window.orizonDebug.frameTimes; nothing else does, and until then it costs the page one test per
// drawn frame. It records two clocks:
// - the display's ticks, from a requestAnimationFrame loop of its own: the page's draw loop asks
//   for a tick only while a frame waits, so it cannot say when the display ticked without one.
//   These are ticks of the page's main thread, not frames on the screen, which WebKit composites
//   in another process.
// - the engine frames the page draws, which are what a user sees move: one every n ticks
//   (render-loop.ts), each judged against the interval the loop aimed for.
import type { Scheduler } from "./render/render-loop.ts";

// A tick interval this many times the median is late: a tick the display skipped. A tick early
// or late by a fraction of its period is not.
export const lateFactor = 1.5;

// What the loop aimed for at a draw: the next frame `interval` ms later, on ticks `tick` ms apart.
// A frame more than half a tick past it was held a tick too long, whatever n is; at one tick a
// frame, that is lateFactor ticks, as for ticks.
export interface FrameAim {
  readonly interval: number;
  readonly tick: number;
}

export interface IntervalSummary {
  // Intervals measured, and the time between successive events at these percentiles, in ms.
  count: number;
  median: number;
  p95: number;
  p99: number;
  max: number;
  // The share of intervals that were late, and the most that ended within any one second.
  lateShare: number;
  worstSecondLate: number;
}

export interface FrameTimesSummary {
  ticks: IntervalSummary;
  frames: IntervalSummary;
}

function percentile(sorted: readonly number[], fraction: number): number {
  if (sorted.length === 0) {
    return Number.NaN;
  }
  const index = Math.round(fraction * (sorted.length - 1));
  return sorted[index] ?? Number.NaN;
}

// The intervals between `times` (ms, rising), judged late beyond lateFactor times `expected`;
// with no expectation, the median interval is the expected one (the display's period, which the
// page cannot ask for).
export function summarise(
  times: readonly number[],
  expected?: number,
): IntervalSummary {
  const intervals: number[] = [];
  const ends: number[] = [];
  for (let index = 1; index < times.length; index++) {
    intervals.push((times[index] ?? 0) - (times[index - 1] ?? 0));
    ends.push(times[index] ?? 0);
  }
  const median = percentile(
    [...intervals].sort((a, b) => a - b),
    0.5,
  );
  const limit = lateFactor * (expected ?? median);
  return summariseIntervals(intervals, ends, () => limit);
}

// The intervals between drawn frames (ms, rising), each judged against the aim of the draw it
// follows. An interval after a draw with no aim (before the loop measured the display) is left
// out: there was nothing to judge it against.
export function summariseFrames(
  times: readonly number[],
  aims: readonly (FrameAim | undefined)[],
): IntervalSummary {
  const intervals: number[] = [];
  const ends: number[] = [];
  const limits: number[] = [];
  for (let index = 1; index < times.length; index++) {
    const aim = aims[index - 1];
    if (aim === undefined) {
      continue;
    }
    intervals.push((times[index] ?? 0) - (times[index - 1] ?? 0));
    ends.push(times[index] ?? 0);
    limits.push(aim.interval + aim.tick / 2);
  }
  return summariseIntervals(intervals, ends, (index) => limits[index] ?? 0);
}

function summariseIntervals(
  intervals: readonly number[],
  ends: readonly number[],
  limitAt: (index: number) => number,
): IntervalSummary {
  const sorted = [...intervals].sort((a, b) => a - b);
  const median = percentile(sorted, 0.5);
  let late = 0;
  let worstSecondLate = 0;
  const lateEnds: number[] = [];
  intervals.forEach((interval, index) => {
    if (!(interval > limitAt(index))) {
      return;
    }
    late++;
    const end = ends[index] ?? 0;
    lateEnds.push(end);
    while ((lateEnds[0] ?? end) <= end - 1000) {
      lateEnds.shift();
    }
    worstSecondLate = Math.max(worstSecondLate, lateEnds.length);
  });
  if (intervals.length === 0) {
    worstSecondLate = Number.NaN; // Nothing measured, not a second with none late.
  }
  return {
    count: intervals.length,
    median,
    p95: percentile(sorted, 0.95),
    p99: percentile(sorted, 0.99),
    max: sorted.at(-1) ?? Number.NaN,
    lateShare: intervals.length === 0 ? Number.NaN : late / intervals.length,
    worstSecondLate,
  };
}

// The newest `capacity` times, in a buffer allocated once.
class TimeRing {
  private readonly values: Float64Array;
  private written = 0;

  constructor(capacity: number) {
    this.values = new Float64Array(capacity);
  }

  clear(): void {
    this.written = 0;
  }

  push(time: number): void {
    this.values[this.written % this.values.length] = time;
    this.written++;
  }

  // Oldest first.
  times(): number[] {
    const kept = Math.min(this.written, this.values.length);
    const first = this.written - kept;
    return Array.from({ length: kept }, (_, index) => {
      return this.values[(first + index) % this.values.length] ?? 0;
    });
  }
}

export class FrameTimes {
  private readonly schedule: Scheduler;
  private readonly capacity: number;
  // Made on the first start, so that a page never benchmarked holds none.
  private ticks: TimeRing | undefined;
  private frames: TimeRing | undefined;
  // Each drawn frame's aim, NaN for none.
  private aimIntervals: TimeRing | undefined;
  private aimTicks: TimeRing | undefined;
  private recording = false;
  private generation = 0; // A stopped loop's last tick does not record into a new one.

  // Enough for nine minutes at 120 Hz, longer than any benchmark run: a run that outlasts it
  // would be summarised over its newest part only.
  constructor(schedule: Scheduler, capacity = 65536) {
    this.schedule = schedule;
    this.capacity = capacity;
  }

  // Starts afresh.
  start(): void {
    const ticks = (this.ticks ??= new TimeRing(this.capacity));
    const frames = (this.frames ??= new TimeRing(this.capacity));
    const aimIntervals = (this.aimIntervals ??= new TimeRing(this.capacity));
    const aimTicks = (this.aimTicks ??= new TimeRing(this.capacity));
    for (const ring of [ticks, frames, aimIntervals, aimTicks]) {
      ring.clear();
    }
    this.recording = true;
    const generation = ++this.generation;
    const tick = (timestamp: number) => {
      if (!this.recording || generation !== this.generation) {
        return;
      }
      ticks.push(timestamp);
      this.schedule(tick);
    };
    this.schedule(tick);
  }

  stop(): void {
    this.recording = false;
  }

  // The draw loop: an engine frame was drawn on the tick at `timestamp`, aiming for the next
  // one as `aim` says (undefined before the loop measured the display).
  drawn(timestamp: number, aim?: FrameAim): void {
    if (this.recording) {
      this.frames?.push(timestamp);
      this.aimIntervals?.push(aim?.interval ?? Number.NaN);
      this.aimTicks?.push(aim?.tick ?? Number.NaN);
    }
  }

  summary(): FrameTimesSummary {
    const intervals = this.aimIntervals?.times() ?? [];
    const ticks = this.aimTicks?.times() ?? [];
    const aims = intervals.map((interval, index) => {
      const tick = ticks[index] ?? Number.NaN;
      return Number.isNaN(interval) || Number.isNaN(tick)
        ? undefined
        : { interval, tick };
    });
    return {
      ticks: summarise(this.ticks?.times() ?? []),
      frames: summariseFrames(this.frames?.times() ?? [], aims),
    };
  }
}

// The page's, on its display's ticks.
export const frameTimes = new FrameTimes((callback) =>
  requestAnimationFrame(callback),
);
