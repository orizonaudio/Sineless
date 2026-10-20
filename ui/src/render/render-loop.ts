// The draw loop: requestAnimationFrame is the only draw clock. Arriving frames only
// replace the latest-frame slot; each tick draws the newest frame once and then returns the
// credit, so the engine sends at most one frame per drawn frame.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { frameTimes } from "../frame-times.ts";
import { perf } from "../perf.ts";

// The draw clock: requestAnimationFrame in the page, a manual clock in tests.
export type Scheduler = (callback: (timestamp: number) => void) => void;

// Which tick a frame is drawn on. Tick timestamps wander (on a 60 Hz MacBook display by up to 2
// to 3 ms either way), and a tick skipped for looking early leaves a frame on screen for two
// ticks, a visible stutter. So the loop measures the display's tick period, as video players
// do, and draws on the tick nearest each frame's time: up to half a tick early. A display no
// faster than this many times the frame rate (66 Hz for 60 frames/s) is not paced at all: every
// tick with a frame waiting draws it.
const pacedAbove = 1.1;
// Before the period is measured, a tick up to this share of the interval early is on time: a
// fifth (3.3 ms at 60 frames/s) takes in most of the wander, and is under half a tick at 120 Hz
// (4.2 ms), whose in-between ticks are skipped.
const unmeasuredTolerance = 0.2;
// The period is the median of this many recent gaps between a draw's tick and the next, leaving
// out stalls (gaps over two intervals), so that one late tick does not move it.
const periodSamples = 15;

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
  private redrawRequested = false;
  // Engine frames are drawn on a schedule of one per `interval` ms, on the display's ticks:
  // at 144 Hz, 60 frames a second fall on ticks 0, 3, 5, 8, 10... A minimum gap since the
  // last draw would instead wait for whole ticks each time, and draw 50 frames a second at
  // 100 Hz, 48 at 144 Hz.
  private readonly interval: number;
  private nextDrawTime = -Infinity;
  // The display's tick period, measured on the tick after each draw (undefined until then).
  private readonly gaps: number[] = [];
  private period: number | undefined;
  private lastDrawTime: number | undefined;

  constructor(
    schedule: Scheduler,
    renderer: FrameRenderer,
    returnCredit: () => void,
    interval = 0,
  ) {
    this.schedule = schedule;
    this.renderer = renderer;
    this.returnCredit = returnCredit;
    this.interval = interval;
  }

  // Called for every frame from the engine.
  submit(frame: Frame): void {
    this.latest = frame;
    this.requestTick();
  }

  // Draws the last frame again on the next tick, e.g. after a display-only setting changed.
  invalidate(): void {
    this.redrawRequested = true;
    this.requestTick();
  }

  private requestTick(): void {
    if (this.tickPending) {
      return;
    }
    this.tickPending = true;
    this.schedule((timestamp) => {
      this.tick(timestamp);
    });
  }

  private tick(timestamp: number): void {
    this.tickPending = false;
    if (this.lastDrawTime !== undefined) {
      this.measure(timestamp - this.lastDrawTime);
      this.lastDrawTime = undefined;
    }
    if (this.latest !== undefined && timestamp < this.drawFrom()) {
      this.requestTick(); // Before the next frame's tick: wait for a later one.
      return;
    }
    const frame = this.latest;
    this.latest = undefined;
    const redraw = this.redrawRequested;
    this.redrawRequested = false;
    if (frame !== undefined) {
      // On schedule; more than a whole interval late (a stall, or the first frame), restarted
      // from now rather than drawing a quick pair to catch up. Not paced, the schedule follows
      // the ticks: a display a little faster than the frame rate would otherwise leave it
      // further ahead at every draw, and stop the frames once the window moves to one paced.
      this.nextDrawTime =
        this.paced() && timestamp - this.nextDrawTime < this.interval
          ? this.nextDrawTime + this.interval
          : timestamp + this.interval;
      const started = performance.now();
      this.renderer.draw(frame);
      const elapsed = performance.now() - started;
      perf.framesDrawn++;
      perf.drawMs += elapsed;
      perf.maxDrawMs = Math.max(perf.maxDrawMs, elapsed);
      frameTimes.drawn(timestamp);
      this.lastDrawn = frame;
      // The next tick, whether or not a frame waits for it, measures the display's period.
      this.lastDrawTime = timestamp;
      this.requestTick();
      this.returnCredit(); // Only engine frames earn a credit; redraws do not.
    } else if (redraw && this.lastDrawn !== undefined) {
      this.renderer.draw(this.lastDrawn);
    }
  }

  // The earliest time a tick draws the frame waiting: the frame's time, less half a tick.
  private drawFrom(): number {
    if (this.period === undefined) {
      return this.nextDrawTime - unmeasuredTolerance * this.interval;
    }
    if (!this.paced()) {
      return -Infinity; // Every tick draws.
    }
    return this.nextDrawTime - this.period / 2;
  }

  // Whether frames are paced to the interval: not on a display measured no faster than
  // pacedAbove times the frame rate.
  private paced(): boolean {
    return (
      this.period === undefined || this.period * pacedAbove < this.interval
    );
  }

  private measure(gap: number): void {
    if (gap <= 0 || gap > 2 * this.interval) {
      return; // A stall, or no schedule to measure against.
    }
    this.gaps.push(gap);
    if (this.gaps.length > periodSamples) {
      this.gaps.shift();
    }
    const sorted = [...this.gaps].sort((a, b) => a - b);
    this.period = sorted[Math.floor(sorted.length / 2)];
  }
}
