// The Beats playhead, as the page draws it: a line straddling device pixels is antialiased dimmer
// and wider by an amount that changes with its subpixel position, so a moving one flickers.
// Started at 2.5x, where both the line's position and a one-CSS-pixel width fall between device
// pixels unless they are snapped, and a line drawn for another ratio has another width.
import {
  afterAll,
  beforeAll,
  describe,
  expect,
  test,
  vi,
  type MockInstance,
} from "vitest";

import { byId, choice, settle, startApp } from "./harness.ts";

interface Quad {
  readonly left: number; // Canvas pixels.
  readonly right: number;
  readonly alpha: number;
}

describe("the Beats playhead at 2.5x", () => {
  const ratio = 2.5;
  let buffers: MockInstance<WebGL2RenderingContext["bufferData"]>;
  let colours: MockInstance<WebGL2RenderingContext["uniform4f"]>;

  beforeAll(async () => {
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      get: () => ratio,
    });
    buffers = vi.spyOn(WebGL2RenderingContext.prototype, "bufferData");
    colours = vi.spyOn(WebGL2RenderingContext.prototype, "uniform4f");
    // The mock starts in Beats, its playhead following the clock: at subpixel positions.
    await startApp();
    expect(choice("time-base", "Beats").getAttribute("aria-pressed")).toBe(
      "true",
    );
    await settle();
  });

  afterAll(() => {
    vi.restoreAllMocks();
  });

  // Each one-colour quad drawn while frames settle: its vertices (one 12-float buffer), then the
  // colour set for it, the next uniform4f call.
  async function drawnQuads(): Promise<{ quads: Quad[]; width: number }> {
    buffers.mockClear();
    colours.mockClear();
    await settle(10);
    const canvas = byId("trace");
    const width = canvas instanceof HTMLCanvasElement ? canvas.width : 0;
    const quads: Quad[] = [];
    buffers.mock.calls.forEach(([, data], index) => {
      if (!(data instanceof Float32Array) || data.length !== 12) {
        return;
      }
      const order = buffers.mock.invocationCallOrder[index] ?? 0;
      const colour = colours.mock.invocationCallOrder.findIndex(
        (call) => call > order,
      );
      const alpha = colours.mock.calls[colour]?.[4];
      if (alpha === undefined) {
        return;
      }
      const pixels = [...data]
        .filter((_, index) => index % 2 === 0)
        .map((clip) => ((clip + 1) / 2) * width);
      quads.push({
        left: Math.min(...pixels),
        right: Math.max(...pixels),
        alpha,
      });
    });
    return { quads, width };
  }

  // The playhead is opaque; the dimmed previous cycle, drawn just before it, runs from it to the
  // screen's right edge.
  async function drawnFrames(): Promise<
    { line: Quad; dimmed?: Quad; width: number }[]
  > {
    const { quads, width } = await drawnQuads();
    const frames: { line: Quad; dimmed?: Quad; width: number }[] = [];
    quads.forEach((quad, index) => {
      if (quad.alpha === 1) {
        const before = quads[index - 1];
        frames.push({
          line: quad,
          width,
          ...(before !== undefined &&
          before.alpha < 1 &&
          Math.abs(before.right - width) < 1e-3
            ? { dimmed: before }
            : {}),
        });
      }
    });
    return frames;
  }

  test("draws the playhead on whole device pixels as it moves, so it does not flicker", async () => {
    const frames = await drawnFrames();
    expect(frames.length).toBeGreaterThan(5);
    // It moved: several positions, not one frame drawn again.
    expect(new Set(frames.map(({ line }) => line.left)).size).toBeGreaterThan(
      1,
    );
    const widths = new Set<string>();
    for (const { line, width: canvasWidth } of frames) {
      expect(line.left).toBeCloseTo(Math.round(line.left), 3);
      expect(line.right).toBeCloseTo(Math.round(line.right), 3);
      // On the screen, or clipped at its edges.
      expect(line.right).toBeGreaterThan(-1e-3);
      expect(line.left).toBeLessThan(canvasWidth + 1e-3);
      widths.add((line.right - line.left).toFixed(3));
    }
    expect(widths.size).toBe(1);
    // A CSS pixel in whole device pixels, as a 1px border is.
    const width = Number([...widths][0]);
    expect(width).toBeGreaterThanOrEqual(1);
    expect(width).toBeLessThanOrEqual(ratio);
    expect(width).toBeGreaterThan(ratio - 1);
  });

  test("starts the dimmed previous cycle on the playhead's pixel boundary", async () => {
    const frames = await drawnFrames();
    const withDimming = frames.filter(({ dimmed }) => dimmed !== undefined);
    expect(withDimming.length).toBeGreaterThan(0);
    for (const { line, dimmed } of withDimming) {
      expect(dimmed?.left).toBeCloseTo(line.left, 3);
    }
  });
});
