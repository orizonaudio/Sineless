// Frame timing for the editor's benchmark (editor/bench/PerfBench.mm), which starts it through
// window.orizonDebug.frameTimes; nothing else does, and until then it costs the page one test per
// drawn frame. It records two clocks:
// - the display's ticks, from a requestAnimationFrame loop of its own: the page's draw loop asks
//   for a tick only while a frame waits, so it cannot say when the display ticked without one.
//   These are ticks of the page's main thread, not frames on the screen, which WebKit composites
//   in another process.
// - the engine frames the page draws, which are what a user sees move: one per tick at most, and
//   at most 60 a second (render-loop.ts).
import type { Scheduler } from "./render/render-loop.ts";

// The pace engine frames are drawn at, in ms: 60 a second on any display rate (at most the
// display's), as more would cost work in every part of the pipeline for motion the eye barely
// tells apart.
export const frameMilliseconds = 1000 / 60;
// An interval this many times the expected one is late: a tick the display skipped, or a frame
// that came a whole frame's worth of ticks after the one before. A tick early or late by a
// fraction of its period, as at 144 Hz, is not; nor is a frame one tick late after a short
// interval (at 100 Hz, 10 ms then 20 ms), which this undercounts.
export const lateFactor = 1.5;

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
  for (let index = 1; index < times.length; index++) {
    intervals.push((times[index] ?? 0) - (times[index - 1] ?? 0));
  }
  const sorted = [...intervals].sort((a, b) => a - b);
  const median = percentile(sorted, 0.5);
  const limit = lateFactor * (expected ?? median);
  let late = 0;
  let worstSecondLate = 0;
  const lateEnds: number[] = [];
  intervals.forEach((interval, index) => {
    if (!(interval > limit)) {
      return;
    }
    late++;
    const end = times[index + 1] ?? 0;
    lateEnds.push(end);
    while ((lateEnds[0] ?? end) <= end - 1000) {
      lateEnds.shift();
    }
    worstSecondLate = Math.max(worstSecondLate, lateEnds.length);
  });
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
    ticks.clear();
    frames.clear();
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

  // The draw loop: an engine frame was drawn on the tick at `timestamp`.
  drawn(timestamp: number): void {
    if (this.recording) {
      this.frames?.push(timestamp);
    }
  }

  summary(): FrameTimesSummary {
    return {
      ticks: summarise(this.ticks?.times() ?? []),
      frames: summarise(this.frames?.times() ?? [], frameMilliseconds),
    };
  }
}

// The page's, on its display's ticks.
export const frameTimes = new FrameTimes((callback) =>
  requestAnimationFrame(callback),
);
