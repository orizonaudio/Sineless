// Turns wheel events into whole steps. A wheel event on its own (a mouse wheel's click) is one
// step, however far it reports scrolling: WebKit reports a few pixels, Windows 100. Events
// in a stream (a trackpad's swipe, a wheel spun fast) step by distance instead, so a swipe of
// dozens of small events gives a few steps rather than dozens.

// Events closer together than this are a stream (ms).
const streamGap = 80;
// In a stream, one step per this distance: one Windows mouse notch (CSS px).
const pixelsPerStep = 100;

export class WheelSteps {
  private pixels = 0;
  private lastTime = -Infinity;
  // Whether the last event continued a stream.
  continues = false;

  // The steps for one event that scrolled `pixels` (positive: down) at `time` (ms): positive
  // for up, as wheels raise values.
  add(pixels: number, time: number): number {
    const isAlone = time - this.lastTime > streamGap;
    this.lastTime = time;
    this.continues = !isAlone;
    if (pixels === 0) {
      return 0;
    }
    if (isAlone) {
      this.pixels = 0;
      return -Math.sign(pixels);
    }
    if (Math.sign(pixels) !== Math.sign(this.pixels)) {
      this.pixels = 0; // Turned around: start over.
    }
    this.pixels += pixels;
    const steps = Math.trunc(this.pixels / pixelsPerStep);
    this.pixels -= steps * pixelsPerStep;
    return steps === 0 ? 0 : -steps;
  }
}
