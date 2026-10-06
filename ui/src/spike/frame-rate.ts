// T-006 spike: measures requestAnimationFrame callbacks per second.

export class FrameRateMeter {
  private windowStart: number | undefined;
  private frames = 0;

  // Records one frame at `timestamp` (ms). Returns the rate once per elapsed second, else undefined.
  tick(timestamp: number): number | undefined {
    if (this.windowStart === undefined) {
      this.windowStart = timestamp;
      return undefined;
    }
    this.frames++;
    const elapsed = timestamp - this.windowStart;
    if (elapsed < 1000) {
      return undefined;
    }
    const rate = (this.frames * 1000) / elapsed;
    this.windowStart = timestamp;
    this.frames = 0;
    return rate;
  }
}
