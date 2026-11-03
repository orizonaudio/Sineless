// The pictures the macOS editor's Metal renderer must match (AD-30): scenes drawn by the page's
// WebGL renderer in WebKit, stored with their frames and trace views in
// protocol/tests/data/trace-pictures.json, which the editor's MetalTraceRendererTests draw again
// and compare. Here the stored pictures are checked against the page's renderer as it is now, so a
// change to its picture fails until the file is written again: ORIZON_UPDATE_VECTORS=1
// npm run test:app, then review the diff and run the editor's tests.
import { afterEach, expect, inject, test } from "vitest";
import { commands, server } from "vitest/browser";

import {
  ChannelView,
  FrameMode,
  ViewLayout,
} from "../../../protocol/src/enums.ts";
import { decodeFrame } from "../../../protocol/src/frame-decoder.ts";
import {
  encodeFrame,
  type FrameContent,
} from "../../../protocol/src/frame-encoder.ts";
import type { Rgba, TraceViewMessage } from "../../../protocol/src/messages.ts";
import {
  TraceRenderer,
  type Rgba as StyleRgba,
} from "../../src/render/trace-renderer.ts";

declare module "vitest" {
  export interface ProvidedContext {
    updateVectors: boolean;
  }
}

// Relative to the project root, as the browser commands take paths.
const file = "protocol/tests/data/trace-pictures.json";
// A level per channel either way: the two GPUs' float arithmetic rounds differently.
const tolerance = 1;

interface Scene {
  readonly name: string;
  readonly width: number; // Device pixels.
  readonly height: number;
  readonly ratio: number;
  readonly frame: FrameContent;
  readonly view: TraceViewMessage;
}

const colour = (r: number, g: number, b: number, a = 1): Rgba => [r, g, b, a];

const baseView: TraceViewMessage = {
  type: "traceView",
  viewStart: 0,
  viewEnd: 1,
  layout: "overlaid",
  verticalScaleDb: 0,
  dim: false,
  lineWidth: 1.5,
  viewColours: [
    colour(0.36, 0.78, 1),
    colour(1, 0.55, 0.3),
    colour(0.6, 1, 0.5, 0.8),
    colour(0.9, 0.5, 1, 0.6),
  ],
  laneSets: [],
  fallbackColours: [colour(0.5, 0.5, 0.5)],
  sumColour: colour(0.95, 0.95, 0.95),
  // Transparent, as the page's trace canvas is: the picture is the traces alone.
  screen: colour(0, 0, 0, 0),
  grid: colour(0.3, 0.3, 0.3),
  axis: colour(0.5, 0.5, 0.5),
  cursor: colour(1, 1, 1),
  previous: colour(0.05, 0.06, 0.08, 0.5),
  cancellation: colour(1, 0.45, 0.2, 0.16),
};

const baseFrame: FrameContent = {
  mode: FrameMode.minMax,
  triggered: false,
  frozen: false,
  freeRunning: false,
  pointCount: 0,
  startFraction: 0,
  peak: 0,
  rms: 0,
  frequency: undefined,
  sampleRate: 48000,
  views: new Map(),
};

// A min/max column pair per point: a sine's envelope with a ripple, so pieces slope both ways.
function columns(
  count: number,
  cycles: number,
  amplitude: number,
  phase = 0,
): number[] {
  return Array.from({ length: count }, (_, index) => {
    const t = (index / count) * cycles * 2 * Math.PI + phase;
    const centre = amplitude * Math.sin(t);
    const spread = 0.05 + 0.08 * Math.abs(Math.cos(3 * t));
    return [centre - spread, centre + spread];
  }).flat();
}

function samples(
  count: number,
  cycles: number,
  amplitude: number,
  phase = 0,
): number[] {
  return Array.from(
    { length: count },
    (_, index) =>
      amplitude * Math.sin((index / count) * cycles * 2 * Math.PI + phase),
  );
}

// Every mode, time base and channel layout the renderer branches on.
const scenes: Scene[] = [
  {
    name: "raw mono, a fractional start",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      mode: FrameMode.raw,
      pointCount: 40,
      startFraction: 0.37,
      visibleSamples: 38.6,
      views: new Map([[ChannelView.left, samples(40, 2, 0.8)]]),
    },
    view: baseView,
  },
  {
    name: "min/max stereo overlaid",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 240,
      views: new Map([
        [ChannelView.left, columns(240, 3, 0.7)],
        [ChannelView.right, columns(240, 3, 0.5, 1)],
      ]),
    },
    view: baseView,
  },
  {
    name: "min/max mid and side stacked at twice the scale",
    width: 480,
    height: 240,
    ratio: 2,
    frame: {
      ...baseFrame,
      pointCount: 480,
      views: new Map([
        [ChannelView.left, columns(480, 2, 0.9)],
        [ChannelView.right, columns(480, 2, 0.6, 2)],
        [ChannelView.mid, columns(480, 4, 0.5)],
        [ChannelView.side, columns(480, 5, 0.3, 1)],
      ]),
    },
    view: { ...baseView, layout: "stacked", lineWidth: 3 },
  },
  {
    name: "raw with missing points and values past the screen",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      mode: FrameMode.raw,
      pointCount: 30,
      views: new Map([
        [
          ChannelView.left,
          samples(30, 1.5, 0.9).map((value, index) =>
            index === 7 || index === 8
              ? Number.NaN
              : index === 15
                ? 1e30
                : index === 22
                  ? Number.POSITIVE_INFINITY
                  : index === 25
                    ? -3
                    : value,
          ),
        ],
      ]),
    },
    view: { ...baseView, verticalScaleDb: -12 },
  },
  {
    name: "a zoom stretching the frame's window into the page's",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 200,
      viewStart: 0.2,
      viewEnd: 0.6,
      views: new Map([[ChannelView.left, columns(200, 2, 0.6)]]),
    },
    view: { ...baseView, viewStart: 0.1, viewEnd: 0.7 },
  },
  {
    name: "Waiting dims the traces",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 240,
      views: new Map([[ChannelView.left, columns(240, 2, 0.8)]]),
    },
    view: { ...baseView, dim: true, lineWidth: 2.5 },
  },
  {
    // The playhead on whole pixels: the page snaps it there, so both draw the same pixels.
    name: "a Beats sweep's playhead on a pixel boundary",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 240,
      synced: true,
      cursor: 100.5 / 240,
      cycleStart: 4,
      spanQuarterNotes: 4,
      barQuarterNotes: 4,
      views: new Map([[ChannelView.left, columns(240, 1, 0.7)]]),
    },
    view: baseView,
  },
  {
    // The page snaps the playhead to whole pixels and the editor does not: the columns it
    // touches are compared by their coverage instead.
    name: "a Beats sweep's playhead between pixels, zoomed",
    width: 480,
    height: 240,
    ratio: 2,
    frame: {
      ...baseFrame,
      pointCount: 480,
      synced: true,
      cursor: 0.4137,
      cycleStart: 0,
      spanQuarterNotes: 16,
      barQuarterNotes: 4,
      viewStart: 0.25,
      viewEnd: 0.75,
      views: new Map([[ChannelView.left, columns(480, 1, 0.7)]]),
    },
    view: { ...baseView, viewStart: 0.25, viewEnd: 0.75, lineWidth: 3 },
  },
  {
    name: "five tracks stacked in their colours",
    width: 240,
    height: 200,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 240,
      laneSet: 3,
      lanes: Array.from({ length: 5 }, (_, lane) =>
        columns(240, lane + 1, 0.8, lane),
      ),
    },
    view: {
      ...baseView,
      layout: "stacked",
      laneSets: [
        {
          laneSet: 3,
          lanes: Array.from({ length: 5 }, (_, lane) => ({
            id: lane,
            colour: colour(0.2 + 0.15 * lane, 1 - 0.15 * lane, 0.5),
            compared: false,
          })),
        },
      ],
    },
  },
  {
    // All four coverage targets.
    name: "sixteen tracks overlaid",
    width: 240,
    height: 120,
    ratio: 1,
    frame: {
      ...baseFrame,
      pointCount: 120,
      laneSet: 9,
      lanes: Array.from({ length: 16 }, (_, lane) =>
        columns(120, 1 + lane / 4, 0.9 - lane * 0.05, lane * 0.4),
      ),
    },
    view: {
      ...baseView,
      laneSets: [
        {
          laneSet: 9,
          lanes: Array.from({ length: 16 }, (_, lane) => ({
            id: lane,
            colour: colour(lane / 15, 1 - lane / 15, 0.6, 0.7),
            compared: false,
          })),
        },
      ],
    },
  },
  {
    // Two compared tracks cancelling in the middle third, a third left out (faint), and the sum.
    name: "Compare shading where the tracks cancel",
    width: 300,
    height: 200,
    ratio: 1,
    frame: (() => {
      const count = 300;
      const first = columns(count, 4, 0.6);
      const second = first.map((value, index) => {
        const column = Math.floor(index / 2);
        return column > count / 3 && column < (2 * count) / 3
          ? -value
          : value * 0.5;
      });
      const sum = first.map((value, index) => value + (second[index] ?? 0));
      return {
        ...baseFrame,
        pointCount: count,
        laneSet: 5,
        lanes: [first, second, columns(count, 2, 0.4), sum],
        comparison: {
          sumGain: 1,
          fixedGain: 1,
          matches: [
            { correlation: 1, related: true, offset: 0, inverted: false },
            { correlation: -1, related: true, offset: 0, inverted: true },
            { correlation: 0, related: false, offset: 0, inverted: false },
          ],
        },
      };
    })(),
    view: {
      ...baseView,
      layout: "stacked",
      laneSets: [
        {
          laneSet: 5,
          lanes: [
            { id: 1, colour: colour(0.4, 0.8, 1), compared: true },
            { id: 2, colour: colour(1, 0.7, 0.3), compared: true },
            { id: 3, colour: colour(0.7, 1, 0.4), compared: false },
          ],
        },
      ],
    },
  },
];

// The page's lane colours for a frame whose lane set the view holds (app.ts, lanesOf and
// laneColors): the Sum last in Compare, tracks left out of it faint.
function laneStyle(
  frame: FrameContent,
  view: TraceViewMessage,
): { colours: StyleRgba[]; compared: boolean[] } {
  const set = view.laneSets.find((each) => each.laneSet === frame.laneSet);
  const comparing = frame.comparison !== undefined;
  const lanes = [
    ...(set?.lanes ?? []),
    ...(comparing ? [{ id: -1, colour: view.sumColour, compared: false }] : []),
  ];
  return {
    colours: lanes.map((lane, index) => {
      const [r = 0, g = 0, b = 0, a = 1] = lane.colour;
      const sum = comparing && index === lanes.length - 1;
      return [r, g, b, comparing && !lane.compared && !sum ? a * 0.2 : a];
    }),
    compared: lanes.map((lane) => lane.compared),
  };
}

const rgba = (value: Rgba): StyleRgba => [
  value[0] ?? 0,
  value[1] ?? 0,
  value[2] ?? 0,
  value[3] ?? 1,
];

const realRatio = window.devicePixelRatio;
afterEach(() => {
  Object.defineProperty(window, "devicePixelRatio", {
    value: realRatio,
    configurable: true,
  });
});

// The page's picture of a scene, premultiplied RGBA rows from the bottom, as WebGL reads them.
function drawScene(scene: Scene): Uint8Array {
  // The playhead's width follows the ratio.
  Object.defineProperty(window, "devicePixelRatio", {
    value: scene.ratio,
    configurable: true,
  });
  const canvas = document.createElement("canvas");
  canvas.width = scene.width;
  canvas.height = scene.height;
  const { view } = scene;
  const lanes = laneStyle(scene.frame, view);
  const renderer = new TraceRenderer(
    canvas,
    {
      colors: () =>
        new Map(
          Object.values(ChannelView).map((channel) => [
            channel,
            rgba(view.viewColours[channel] ?? [1, 1, 1, 1]),
          ]),
        ),
      laneColors: () => lanes.colours,
      comparedLanes: () => lanes.compared,
      sweepColors: () => ({
        cursor: rgba(view.cursor),
        previous: rgba(view.previous),
      }),
      cancellationColor: () => rgba(view.cancellation),
      lineWidthPixels: view.lineWidth,
    },
    () => ({
      verticalScaleDb: view.verticalScaleDb,
      layout:
        view.layout === "stacked" ? ViewLayout.stacked : ViewLayout.overlaid,
      dim: view.dim,
      view: { start: view.viewStart, end: view.viewEnd },
    }),
  );
  renderer.draw(decodeFrame(encodeFrame(scene.frame)));
  const gl = canvas.getContext("webgl2");
  if (gl === null) {
    throw new Error("No WebGL2 context");
  }
  const pixels = new Uint8Array(scene.width * scene.height * 4);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.readPixels(
    0,
    0,
    scene.width,
    scene.height,
    gl.RGBA,
    gl.UNSIGNED_BYTE,
    pixels,
  );
  gl.getExtension("WEBGL_lose_context")?.loseContext();
  return pixels;
}

function base64(bytes: Uint8Array): string {
  let text = "";
  for (let at = 0; at < bytes.length; at += 0x8000) {
    text += String.fromCharCode(...bytes.subarray(at, at + 0x8000));
  }
  return btoa(text);
}

function unbase64(text: string): Uint8Array {
  return Uint8Array.from(atob(text), (character) => character.charCodeAt(0));
}

async function gzip(bytes: Uint8Array, format: "compress" | "decompress") {
  const stream = new Blob([bytes as BlobPart])
    .stream()
    .pipeThrough(
      format === "compress"
        ? new CompressionStream("gzip")
        : new DecompressionStream("gzip"),
    );
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

interface StoredScene {
  readonly name: string;
  readonly width: number;
  readonly height: number;
  readonly ratio: number;
  readonly frame: string;
  readonly traceView: TraceViewMessage;
  readonly pixels: string;
}

// WebKit draws as WKWebView does, which the macOS editor replaces.
test.runIf(server.browser === "webkit")(
  "the stored pictures are the page's renderer's",
  async () => {
    const drawn = scenes.map((scene) => ({
      scene,
      pixels: drawScene(scene),
    }));
    if (inject("updateVectors")) {
      const stored: StoredScene[] = await Promise.all(
        drawn.map(async ({ scene, pixels }) => ({
          name: scene.name,
          width: scene.width,
          height: scene.height,
          ratio: scene.ratio,
          frame: base64(new Uint8Array(encodeFrame(scene.frame))),
          traceView: scene.view,
          pixels: base64(await gzip(pixels, "compress")),
        })),
      );
      await commands.writeFile(
        file,
        `${JSON.stringify(
          {
            about:
              "Pictures the macOS editor's Metal renderer must match: each scene's frame (base64) and trace view drawn by the page's WebGL renderer in WebKit, as premultiplied RGBA rows from the bottom, gzipped and base64. Written by ui/tests/app/trace-pictures.test.ts with ORIZON_UPDATE_VECTORS=1; read by editor/tests/MetalTraceRendererTests.mm.",
            scenes: stored,
          },
          null,
          2,
        )}\n`,
      );
      return;
    }
    const stored = (
      JSON.parse(await commands.readFile(file)) as { scenes: StoredScene[] }
    ).scenes;
    expect(stored.map((scene) => scene.name)).toEqual(
      scenes.map((scene) => scene.name),
    );
    for (const [index, { scene, pixels }] of drawn.entries()) {
      const expected = await gzip(
        unbase64(stored[index]?.pixels ?? ""),
        "decompress",
      );
      expect(expected.length, scene.name).toBe(pixels.length);
      let worst = 0;
      for (let at = 0; at < pixels.length; at++) {
        worst = Math.max(
          worst,
          Math.abs((pixels[at] ?? 0) - (expected[at] ?? 0)),
        );
      }
      expect(worst, scene.name).toBeLessThanOrEqual(tolerance);
    }
  },
);
