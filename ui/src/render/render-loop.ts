// The draw loop: requestAnimationFrame is the only draw clock. Arriving frames only
// replace the latest-frame slot; each tick draws the newest frame once and then returns the
// credit, so the engine sends at most one frame per drawn frame.
import type { Frame } from "../../../protocol/src/frame-decoder.ts";

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
    this.schedule(() => {
      this.tick();
    });
  }

  private tick(): void {
    this.tickPending = false;
    const frame = this.latest;
    this.latest = undefined;
    const redraw = this.redrawRequested;
    this.redrawRequested = false;
    if (frame !== undefined) {
      this.renderer.draw(frame);
      this.lastDrawn = frame;
      this.returnCredit(); // Only engine frames earn a credit; redraws do not.
    } else if (redraw && this.lastDrawn !== undefined) {
      this.renderer.draw(this.lastDrawn);
    }
  }
}
