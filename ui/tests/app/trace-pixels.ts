// Draws one frame with the real trace renderer on a canvas of its own and reads back what it drew,
// for tests of the trace's shape. The pixels are read straight after the draw, before the page
// composites the canvas and its drawing buffer is cleared.
import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../../protocol/src/enums.ts";
import {
  decodeFrame,
  type Frame,
} from "../../../protocol/src/frame-decoder.ts";
import {
  encodeFrame,
  type FrameContent,
} from "../../../protocol/src/frame-encoder.ts";
import {
  TraceRenderer,
  type Rgba,
  type TraceView,
} from "../../src/render/trace-renderer.ts";
import type { ViewWindow } from "../../src/view/zoom.ts";

export interface Drawing {
  readonly width: number; // Device pixels.
  readonly height: number;
  readonly frame: Frame;
  // The GL error after the draw (gl.NO_ERROR when none).
  readonly error: number;
  // Alpha (0 to 1) of the pixel at x, y; y counts up from the bottom, as clip space does.
  alpha(x: number, y: number): number;
  // Red, green, blue and alpha (0 to 255, premultiplied) of the pixel at x, y.
  pixel(x: number, y: number): readonly [number, number, number, number];
}

export interface DrawOptions {
  readonly width?: number;
  readonly height?: number;
  readonly lineWidthPixels?: number;
  readonly color?: Rgba;
  readonly laneColors?: readonly Rgba[];
  // A Compare frame's compared lanes, which shade where they cancel.
  readonly comparedLanes?: readonly boolean[];
  readonly layout?: ViewLayout;
  readonly dim?: boolean;
  readonly view?: ViewWindow;
  readonly verticalScaleDb?: number;
}

// One renderer per canvas size: a page holds only a few WebGL contexts.
export interface Surface {
  readonly canvas: HTMLCanvasElement;
  readonly renderer: TraceRenderer;
}
const surfaces = new Map<string, Surface>();
let style: Required<
  Pick<
    DrawOptions,
    "lineWidthPixels" | "color" | "laneColors" | "comparedLanes"
  >
> = {
  lineWidthPixels: 3,
  color: [1, 1, 1, 1],
  laneColors: [],
  comparedLanes: [],
};
let view: TraceView = { verticalScaleDb: 0, layout: ViewLayout.overlaid };

function surface(width: number, height: number): Surface {
  const key = `${String(width)}x${String(height)}`;
  const existing = surfaces.get(key);
  if (existing !== undefined) {
    return existing;
  }
  const made = freshSurface(width, height);
  surfaces.set(key, made);
  return made;
}

// A renderer of its own, for a test that resizes its canvas.
export function freshSurface(width: number, height: number): Surface {
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const renderer = new TraceRenderer(
    canvas,
    {
      colors: () =>
        new Map(Object.values(ChannelView).map((v) => [v, style.color])),
      laneColors: () => style.laneColors,
      comparedLanes: () => style.comparedLanes,
      sweepColors: () => ({ cursor: [1, 1, 1, 1], previous: [0, 0, 0, 0.5] }),
      cancellationColor: () => [1, 0, 0, 0.16],
      get lineWidthPixels() {
        return style.lineWidthPixels;
      },
    },
    () => view,
  );
  return { canvas, renderer };
}

// A frame with the header fields a drawing test does not care about filled in.
export function frameOf(
  content: Partial<FrameContent> & Pick<FrameContent, "mode" | "pointCount">,
): Frame {
  return decodeFrame(
    encodeFrame({
      triggered: false,
      frozen: false,
      roll: false,
      startFraction: 0,
      peak: 0,
      rms: 0,
      frequency: undefined,
      sampleRate: 48000,
      views: new Map(),
      ...content,
    }),
  );
}

// One trace (the left view) of min/max pairs or raw values.
export function traceFrame(
  mode: FrameMode,
  values: readonly number[],
  content: Partial<FrameContent> = {},
): Frame {
  return frameOf({
    mode,
    pointCount: mode === FrameMode.minMax ? values.length / 2 : values.length,
    views: new Map([[ChannelView.left, values]]),
    ...content,
  });
}

export function draw(frame: Frame, options: DrawOptions = {}): Drawing {
  return drawOn(
    surface(options.width ?? 400, options.height ?? 200),
    frame,
    options,
  );
}

// Draws on a given renderer at its canvas's current size.
export function drawOn(
  { canvas, renderer }: Surface,
  frame: Frame,
  options: DrawOptions = {},
): Drawing {
  const { width, height } = canvas;
  style = {
    lineWidthPixels: options.lineWidthPixels ?? 3,
    color: options.color ?? [1, 1, 1, 1],
    laneColors: options.laneColors ?? [],
    comparedLanes: options.comparedLanes ?? [],
  };
  view = {
    verticalScaleDb: options.verticalScaleDb ?? 0,
    layout: options.layout ?? ViewLayout.overlaid,
    ...(options.dim === undefined ? {} : { dim: options.dim }),
    ...(options.view === undefined ? {} : { view: options.view }),
  };
  renderer.draw(frame);
  const gl = canvas.getContext("webgl2");
  if (gl === null) {
    throw new Error("No WebGL2 context");
  }
  const pixels = new Uint8Array(width * height * 4);
  // The canvas, as the screen shows it, whatever the renderer left bound.
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
  const error = gl.getError();
  return {
    width,
    height,
    frame,
    error,
    alpha: (x, y) =>
      x < 0 || y < 0 || x >= width || y >= height
        ? 0
        : (pixels[(y * width + x) * 4 + 3] ?? 0) / 255,
    pixel: (x, y) => {
      const at = (y * width + x) * 4;
      return [
        pixels[at] ?? 0,
        pixels[at + 1] ?? 0,
        pixels[at + 2] ?? 0,
        pixels[at + 3] ?? 0,
      ];
    },
  };
}

// Coverage summed down pixel column x: the trace's vertical thickness there, in pixels.
export function columnCoverage(drawing: Drawing, x: number): number {
  let sum = 0;
  for (let y = 0; y < drawing.height; y++) {
    sum += drawing.alpha(x, y);
  }
  return sum;
}

// Clip-space y of a value at 0 dB full scale in an overlaid lane, in device pixels from the bottom.
export function pixelY(drawing: Drawing, value: number): number {
  return ((value + 1) / 2) * drawing.height;
}

// Clip-space x of a span position under the view, in device pixels from the left.
export function pixelX(
  drawing: Drawing,
  position: number,
  screen: ViewWindow = { start: 0, end: 1 },
): number {
  return (
    ((position - screen.start) / (screen.end - screen.start)) * drawing.width
  );
}
