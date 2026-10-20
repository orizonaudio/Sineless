// The draw loop: requestAnimationFrame is the only draw clock. Arriving frames only
// replace the latest-frame slot; each tick draws the newest frame once and then returns the
// credit, so the engine sends at most one frame per drawn frame.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { frameTimes } from "../frame-times.ts";
import { perf } from "../perf.ts";

// The draw clock: requestAnimationFrame in the page, a manual clock in tests.
export type Scheduler = (callback: (timestamp: number) => void) => void;

// How early a tick may come, as a share of the interval, and still be the tick to draw on. Tick
// timestamps wander: on a 60 Hz MacBook display by up to 2 to 3 ms either way, and a tick
// skipped for coming a little early leaves a frame on screen for two ticks. A quarter of a 60
// frames/s interval (4.2 ms) takes in that wander, and is still under half a tick at 120 Hz,
// whose in-between ticks come half an interval early and are skipped.
const tickTolerance = 0.25;

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
    if (
      this.latest !== undefined &&
      timestamp < this.nextDrawTime - tickTolerance * this.interval
    ) {
      this.requestTick(); // Before the next frame's time: wait for a later tick.
      return;
    }
    const frame = this.latest;
    this.latest = undefined;
    const redraw = this.redrawRequested;
    this.redrawRequested = false;
    if (frame !== undefined) {
      // On schedule; more than a whole interval late (a stall, or the first frame), restarted
      // from now rather than drawing a quick pair to catch up.
      this.nextDrawTime =
        timestamp - this.nextDrawTime < this.interval
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
      this.returnCredit(); // Only engine frames earn a credit; redraws do not.
    } else if (redraw && this.lastDrawn !== undefined) {
      this.renderer.draw(this.lastDrawn);
    }
  }
}
