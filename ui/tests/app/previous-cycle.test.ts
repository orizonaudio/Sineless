// In Beats, the previous cycle right of the playhead shows its traces, and Compare's shading,
// dimmed, and nothing else: drawn by the page's renderer, every pixel left of the line is as it is
// without a playhead, and every pixel right of it keeps its coverage, its colour moved
// previousCycleAlpha of the way from the screen's. So the grid under the trace canvas shows
// unchanged where nothing is drawn, and a dimmed trace covers it as a live one does (FR-9.2).
import { afterEach, describe, expect, test } from "vitest";

import { FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { FrameContent } from "../../../protocol/src/frame-encoder.ts";
import { previousCycleAlpha } from "../../../protocol/src/limits.ts";
import { playheadEdges } from "../../src/render/trace-geometry.ts";
import type { Rgba } from "../../src/render/trace-renderer.ts";
import type { ViewWindow } from "../../src/view/zoom.ts";
import { displays, type Display } from "./harness.ts";
import {
  draw,
  frameOf,
  screenColor,
  type DrawOptions,
} from "./trace-pixels.ts";

const width = 400;
const height = 200;
const count = 100;
const realRatio = window.devicePixelRatio;

afterEach(() => {
  Object.defineProperty(window, "devicePixelRatio", {
    value: realRatio,
    configurable: true,
  });
});

// Filled bands, the same at every point, so each column is the same without a playhead; in
// Overlaid the lanes overlap, so dimming the traces as one group differs from dimming each.
const band = (half: number) =>
  Array.from({ length: count * 2 }, (_, k) => (k % 2 === 0 ? -half : half));
const colour = (lane: number): Rgba => [
  0.2 + lane * 0.3,
  0.9 - lane * 0.3,
  0.6,
  0.8,
];

// Two tracks that cancel in their sum (shaded behind it, as Compare draws), two tracks and a
// third in Multi, two channel views in Single.
function contentOf(display: Display): Partial<FrameContent> {
  if (display === "Single") {
    return {
      views: new Map([
        [0, band(0.5)],
        [1, band(0.3)],
      ]),
    };
  }
  const lanes = [band(0.5), band(0.4), band(0.05)];
  if (display === "Multi") {
    return { lanes };
  }
  const own = { correlation: 0, related: false, offset: 0, inverted: false };
  const alike = { correlation: 0.9, related: true, offset: 0, inverted: false };
  return {
    lanes,
    comparison: { sumGain: -3, fixedGain: 3, matches: [own, alike, alike] },
  };
}

// The playhead at the left edge, mid-screen, at the right edge, and zoomed with it left of the view
// (all of the screen the previous cycle) and right of it (none).
const zoomed: ViewWindow = { start: 0.25, end: 0.75 };
const placements: { name: string; cursor: number; view?: ViewWindow }[] = [
  { name: "at the left edge", cursor: 0 },
  { name: "mid-screen", cursor: 0.5 },
  { name: "between pixels", cursor: 0.4137 },
  { name: "at the right edge", cursor: 1 },
  { name: "left of a zoomed view", cursor: 0.1, view: zoomed },
  { name: "right of a zoomed view", cursor: 0.9, view: zoomed },
];
const layouts = Object.keys(ViewLayout) as (keyof typeof ViewLayout)[];
const ratios = [1, 1.25, 2];

type Pixel = readonly [number, number, number, number];

// A pixel (premultiplied, 0 to 255) of the traces and shading with every colour moved keep of the
// way from the screen's, coverage kept: each layer's colour moves, which is linear, so the whole
// pixel moves, its alpha unchanged.
function dimmed(pixel: Pixel, keep: number): Pixel {
  const alpha = pixel[3] / 255;
  const channel = (k: number) =>
    keep * (pixel[k] ?? 0) + (1 - keep) * (screenColor[k] ?? 0) * 255 * alpha;
  return [channel(0), channel(1), channel(2), pixel[3]];
}

describe("the previous cycle dims its traces only", () => {
  test.each(
    displays.flatMap((display) =>
      layouts.flatMap((layout) =>
        placements.flatMap((placement) =>
          ratios.map((ratio) => ({ display, layout, placement, ratio })),
        ),
      ),
    ),
  )(
    "$display, $layout, the playhead $placement.name, ratio $ratio",
    ({ display, layout, placement, ratio }) => {
      Object.defineProperty(window, "devicePixelRatio", {
        value: ratio,
        configurable: true,
      });
      const content = {
        mode: FrameMode.minMax,
        pointCount: count,
        viewStart: 0,
        viewEnd: 1,
        ...contentOf(display),
      };
      const options: DrawOptions = {
        color: colour(0),
        laneColors: [0, 1, 2].map(colour),
        comparedLanes: display === "Compare" ? [true, true] : [],
        layout: ViewLayout[layout],
        ...(placement.view === undefined ? {} : { view: placement.view }),
      };
      const without = draw(frameOf(content), options);
      // Compare's shading alone, to see that the scene has some to dim.
      const shadingOnly = draw(frameOf(content), {
        ...options,
        color: [0, 0, 0, 0],
        laneColors: [0, 1, 2].map((): Rgba => [0, 0, 0, 0]),
      });
      const withPlayhead = draw(
        frameOf({ ...content, synced: true, cursor: placement.cursor }),
        options,
      );
      expect(withPlayhead.error).toBe(0);
      const screen = placement.view ?? { start: 0, end: 1 };
      const position =
        (placement.cursor - screen.start) / (screen.end - screen.start);
      const toPixels = (clip: number) => Math.round(((clip + 1) / 2) * width);
      const edges = playheadEdges(position, width, ratio);
      const line =
        position >= 0 && position <= 1
          ? { left: toPixels(edges.left), right: toPixels(edges.right) }
          : undefined;
      const dimmedFrom = position < 1 ? toPixels(edges.left) : Infinity;

      const wrong: string[] = [];
      let traced = 0;
      let shaded = 0;
      for (let x = 0; x < width; x++) {
        if (line !== undefined && x >= line.left && x < line.right) {
          continue;
        }
        const keep = x >= dimmedFrom ? previousCycleAlpha : 1;
        for (let y = 0; y < height; y++) {
          const before = without.pixel(x, y);
          const after = withPlayhead.pixel(x, y);
          traced += before[3] > 128 ? 1 : 0;
          shaded += shadingOnly.pixel(x, y)[3] > 0 ? 1 : 0;
          const expected = dimmed(before, keep);
          if (
            expected.some((value, k) => Math.abs(value - (after[k] ?? 0)) > 2)
          ) {
            wrong.push(
              `(${String(x)}, ${String(y)}) ${before.join(",")} → ${after.join(",")}`,
            );
          }
        }
      }
      // The scene draws something to dim.
      expect(traced).toBeGreaterThan(width);
      // In Compare, shading as well, dimmed with the traces.
      expect(shaded > 0).toBe(display === "Compare");
      expect(wrong.slice(0, 5), `${String(wrong.length)} pixels`).toEqual([]);
    },
  );
});
