// The draw loop: requestAnimationFrame is the only draw clock. Arriving frames only
// replace the latest-frame slot; each tick draws the newest frame once and then returns the
// credit, so the engine sends at most one frame per drawn frame.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { perf } from "../perf.ts";

// The draw clock: requestAnimationFrame in the page, a manual clock in tests.
export type Scheduler = (callback: (timestamp: number) => void) => void;

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
  // Engine frames are drawn at most once per `minimumInterval` ms; ticks in between pass.
  private readonly minimumInterval: number;
  private lastDrawTime = -Infinity;

  constructor(
    schedule: Scheduler,
    renderer: FrameRenderer,
    returnCredit: () => void,
    minimumInterval = 0,
  ) {
    this.schedule = schedule;
    this.renderer = renderer;
    this.returnCredit = returnCredit;
    this.minimumInterval = minimumInterval;
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
      timestamp - this.lastDrawTime < this.minimumInterval
    ) {
      this.requestTick(); // Too soon after the last frame: wait for a later tick.
      return;
    }
    const frame = this.latest;
    this.latest = undefined;
    const redraw = this.redrawRequested;
    this.redrawRequested = false;
    if (frame !== undefined) {
      this.lastDrawTime = timestamp;
      const started = performance.now();
      this.renderer.draw(frame);
      const elapsed = performance.now() - started;
      perf.framesDrawn++;
      perf.drawMs += elapsed;
      perf.maxDrawMs = Math.max(perf.maxDrawMs, elapsed);
      this.lastDrawn = frame;
      this.returnCredit(); // Only engine frames earn a credit; redraws do not.
    } else if (redraw && this.lastDrawn !== undefined) {
      this.renderer.draw(this.lastDrawn);
    }
  }
}
