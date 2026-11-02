// The draw loop: requestAnimationFrame is the only draw clock. Arriving frames only
// replace the latest-frame slot; a tick draws the newest frame once and then returns the
// credit, so the engine sends at most one frame per drawn frame.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { frameTimes } from "../frame-times.ts";
import { perf } from "../perf.ts";

// The draw clock: requestAnimationFrame in the page, a manual clock in tests.
export type Scheduler = (callback: (timestamp: number) => void) => void;

// Engine frames are drawn on every n-th tick, n the smallest whole number keeping the tick rate
// divided by n at or below this, so each frame stays on screen for the same n ticks (#308): a
// frame held for 2, 1, 2 ticks while the playhead moves by the time between frames judders.
// Every tick up to 90 Hz, 50 frames a second at 100 Hz, 72 at 144 Hz. 95 Hz lies between the
// common rates 90 and 100 Hz, and its multiples (190, 285, 380 Hz) between theirs, so a measured
// rate a little off never puts a common display on the wrong side.
export const fullRateUpToHz = 95;
// Once n is set, it changes only when the measured rate is this share past a line, so a rate
// measured near one does not change n back and forth. The common rates lie further from the
// lines (5%).
export const lineHysteresis = 0.02;
// Gaps this long are stalls, not ticks (a 20 Hz tick or slower), and are not measured.
const stallMilliseconds = 50;
// The reference period: the median of this many recent gaps. It follows a move to another
// display within about 8 gaps, and n is not set until it holds this many.
const referenceSamples = 15;
// The period: the mean of this many recent gaps within outlierShare of the reference. A mean of
// many gaps averages out the timestamps' wander (a median of 15 left n wrong on up to 12% of
// frames at 100 Hz); the share keeps skipped ticks (twice the period) out.
const periodSamples = 120;
const outlierShare = 0.45;
// When the reference moves this far from the period (a move to another display), the period
// starts again from the new gaps.
const restartShare = 0.25;

// How many ticks each frame stays for at `rateHz` ticks a second. With `previous`, n changes only
// once the rate is past a line by the hysteresis.
export function ticksPerFrame(rateHz: number, previous?: number): number {
  const plain = Math.max(1, Math.ceil(rateHz / fullRateUpToHz));
  if (previous === undefined) {
    return plain;
  }
  const up = Math.max(
    1,
    Math.ceil(rateHz / (fullRateUpToHz * (1 + lineHysteresis))),
  );
  const down = Math.max(
    1,
    Math.ceil(rateHz / (fullRateUpToHz * (1 - lineHysteresis))),
  );
  if (up > previous) {
    return up;
  }
  if (down < previous) {
    return down;
  }
  return previous;
}

export interface FrameRenderer {
  draw(frame: Frame): void;
}

export class RenderLoop {
  private readonly schedule: Scheduler;
  private readonly renderer: FrameRenderer;
  private readonly returnCredit: () => void;
  private latest: Frame | undefined;
  private lastDrawn: Frame | undefined;
  private tickPending = false;
  // Whether the pending tick was asked for by a tick: only then is the gap between them one
  // the display made, and only then is it measured.
  private chained = false;
  private redrawRequested = false;
  // Ticks since the last engine frame was drawn. A frame is drawn once it reaches n: counted,
  // not timed, so the timestamps' wander cannot hold a frame a tick too long or too short.
  private ticksSinceDraw = Infinity;
  private lastTickTime: number | undefined;
  private n: number | undefined;
  private readonly recent: number[] = [];
  private readonly kept: number[] = [];
  private keptSum = 0;
  private period: number | undefined;

  constructor(
    schedule: Scheduler,
    renderer: FrameRenderer,
    returnCredit: () => void,
  ) {
    this.schedule = schedule;
    this.renderer = renderer;
    this.returnCredit = returnCredit;
  }

  // Called for every frame from the engine.
  submit(frame: Frame): void {
    this.latest = frame;
    this.requestTick(false);
  }

  // Draws the last frame again on the next tick, e.g. after a display-only setting changed.
  invalidate(): void {
    this.redrawRequested = true;
    this.requestTick(false);
  }

  private requestTick(fromTick: boolean): void {
    if (this.tickPending) {
      return;
    }
    this.tickPending = true;
    this.chained = fromTick;
    this.schedule((timestamp) => {
      this.tick(timestamp);
    });
  }

  private tick(timestamp: number): void {
    this.tickPending = false;
    let ticks = 1;
    if (this.chained && this.lastTickTime !== undefined) {
      const gap = timestamp - this.lastTickTime;
      this.measure(gap);
      if (gap >= stallMilliseconds) {
        ticks = Number.POSITIVE_INFINITY; // A stall spans more ticks than any frame waits.
      }
    }
    this.lastTickTime = timestamp;
    this.ticksSinceDraw += ticks;
    const n = this.n ?? 1;
    if (this.latest !== undefined && this.ticksSinceDraw >= n) {
      const frame = this.latest;
      this.latest = undefined;
      this.redrawRequested = false;
      this.ticksSinceDraw = 0;
      const aim =
        this.period === undefined || this.n === undefined
          ? undefined
          : { interval: this.n * this.period, tick: this.period };
      const started = performance.now();
      try {
        this.renderer.draw(frame);
      } finally {
        // A draw that throws still counts its ticks and asks for the next frame: nothing else
        // would.
        this.requestTick(true);
        this.returnCredit(); // Only engine frames earn a credit; redraws do not.
      }
      const elapsed = performance.now() - started;
      perf.framesDrawn++;
      perf.drawMs += elapsed;
      perf.maxDrawMs = Math.max(perf.maxDrawMs, elapsed);
      frameTimes.drawn(timestamp, aim);
      this.lastDrawn = frame;
      return;
    }
    if (this.latest === undefined && this.redrawRequested) {
      // A redraw asked for while a frame waits for its tick waits for that frame's draw.
      this.redrawRequested = false;
      if (this.lastDrawn !== undefined) {
        this.renderer.draw(this.lastDrawn);
      }
    }
    // Count every tick up to the next frame's turn, and wait for a frame that has not come.
    if (this.latest !== undefined || this.ticksSinceDraw < n) {
      this.requestTick(true);
    }
  }

  private measure(gap: number): void {
    if (!(gap > 0) || gap >= stallMilliseconds) {
      return;
    }
    this.recent.push(gap);
    if (this.recent.length > referenceSamples) {
      this.recent.shift();
    }
    const sorted = [...this.recent].sort((a, b) => a - b);
    const reference = sorted[Math.floor(sorted.length / 2)] ?? gap;
    if (
      this.period !== undefined &&
      Math.abs(reference - this.period) > restartShare * this.period
    ) {
      this.kept.length = 0;
      this.keptSum = 0;
    }
    if (Math.abs(gap - reference) <= outlierShare * reference) {
      this.kept.push(gap);
      this.keptSum += gap;
      if (this.kept.length > periodSamples) {
        this.keptSum -= this.kept.shift() ?? 0;
      }
    }
    this.period =
      this.kept.length > 0 ? this.keptSum / this.kept.length : reference;
    if (this.recent.length >= referenceSamples) {
      this.n = ticksPerFrame(1000 / this.period, this.n);
    }
  }
}
