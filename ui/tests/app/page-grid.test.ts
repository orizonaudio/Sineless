// The grid the page draws under its own traces (WebGL mode: Windows, and macOS before 14): the time
// divisions, each lane's level lines and its zero line, in the grid and axis colours, where the
// shared geometry puts them, and nothing left of a layout it no longer shows.
import { afterEach, expect, test } from "vitest";

import { gridLines } from "../../src/view/grid.ts";
import {
  byId,
  choice,
  inTimeBase,
  press,
  settle,
  startApp,
} from "./harness.ts";

function tokenRgb(name: string): number[] {
  const hex = getComputedStyle(document.documentElement)
    .getPropertyValue(name)
    .trim()
    .replace("#", "");
  return [0, 2, 4].map((offset) => parseInt(hex.slice(offset, offset + 2), 16));
}

function gridPixels(): {
  canvas: HTMLCanvasElement;
  at: (x: number, y: number) => number[];
} {
  const canvas = byId("grid") as HTMLCanvasElement;
  const context = canvas.getContext("2d");
  expect(context).not.toBeNull();
  const image = context?.getImageData(0, 0, canvas.width, canvas.height);
  return {
    canvas,
    at: (x, y) => {
      const index = (Math.floor(y) * canvas.width + Math.floor(x)) * 4;
      return Array.from(image?.data.slice(index, index + 4) ?? []);
    },
  };
}

// Where the shared geometry puts each line, the page has drawn it, in its own colour: a line's
// middle pixel is mostly covered, and its colour is the token's.
function expectLinesDrawn(laneCount: number): void {
  const { canvas, at } = gridPixels();
  const lines = gridLines(
    canvas.width,
    canvas.height,
    laneCount,
    window.devicePixelRatio,
  );
  const grid = tokenRgb("--sl-grid");
  const axis = tokenRgb("--sl-grid-strong");
  const row = canvas.height / (2 * laneCount) + 5;
  for (const column of lines.vertical) {
    const pixel = at(column.x, row);
    expect(pixel[3], `column at ${String(column.x)}`).toBeGreaterThan(127);
    expect(pixel.slice(0, 3)).toEqual(column.strong ? axis : grid);
  }
  for (const line of lines.horizontal) {
    if (line.y >= canvas.height) {
      continue;
    }
    const pixel = at(3, line.y);
    expect(pixel[3], `row at ${String(line.y)}`).toBeGreaterThan(127);
    expect(pixel.slice(0, 3)).toEqual(line.axis ? axis : grid);
  }
}

test("the page draws its grid where the lines belong, in their colours, and clears the old one", async () => {
  await startApp();
  await settle();
  await inTimeBase("Trig");
  await press(choice("layout", "Stack"));
  await settle();
  expectLinesDrawn(2);

  // Overlaid, one lane: the stacked lanes' lines that the single lane has not are gone, not left
  // under the new grid.
  const { canvas } = gridPixels();
  const stacked = gridLines(
    canvas.width,
    canvas.height,
    2,
    window.devicePixelRatio,
  );
  const overlaid = gridLines(
    canvas.width,
    canvas.height,
    1,
    window.devicePixelRatio,
  );
  const gone = stacked.horizontal.filter(
    (line) =>
      line.y < canvas.height &&
      overlaid.horizontal.every((kept) => Math.abs(kept.y - line.y) > 3),
  );
  expect(gone.length).toBeGreaterThan(0);
  await press(choice("layout", "Overlay"));
  await settle();
  expectLinesDrawn(1);
  const { at } = gridPixels();
  for (const line of gone) {
    expect(at(3, line.y)[3], `old line at ${String(line.y)}`).toBe(0);
  }
});

// At every display scale each line is crisp: whole device pixels of its colour, as many as its
// width, and nothing partly covered beside it. A line of even width centred mid-pixel would be
// spread over three pixels, its full colour only one pixel wide, and look faint (a Retina display
// at scale 2).
const realRatio = window.devicePixelRatio;
afterEach(() => {
  Object.defineProperty(window, "devicePixelRatio", {
    value: realRatio,
    configurable: true,
  });
});

test.each([1, 1.25, 1.5, 2, 2.5])(
  "the page draws each grid line crisp at scale %s",
  async (scale) => {
    Object.defineProperty(window, "devicePixelRatio", {
      value: scale,
      configurable: true,
    });
    await startApp();
    await inTimeBase("Trig");
    await press(choice("layout", "Stack"));
    await settle();
    const { canvas, at } = gridPixels();
    const lines = gridLines(canvas.width, canvas.height, 2, scale);
    const width = Math.max(1, Math.round(scale));
    // The pixels across a line, centred at `centre`, inside the canvas (`size` long): all whole,
    // and as many of its colour as its band has inside the canvas (the 0 dB lines on the canvas's
    // edges show only their inner part).
    const across = (
      centre: number,
      size: number,
      pixelAt: (offset: number) => number[],
      name: string,
    ) => {
      const from = Math.max(0, Math.floor(centre - width / 2) - 1);
      const to = Math.min(size, Math.ceil(centre + width / 2) + 1);
      const pixels = Array.from({ length: to - from }, (_, k) =>
        pixelAt(from + k),
      );
      const partial = pixels.filter(
        (pixel) => (pixel[3] ?? 0) > 0 && (pixel[3] ?? 0) < 255,
      );
      expect(partial, `${name}: partly covered pixels`).toEqual([]);
      const inside = Math.max(
        0,
        Math.min(size, centre + width / 2) - Math.max(0, centre - width / 2),
      );
      expect(
        pixels.filter((pixel) => pixel[3] === 255).length,
        `${name}: pixels of its colour`,
      ).toBe(Math.round(inside));
    };
    expect(lines.vertical.length).toBeGreaterThan(0);
    const row = canvas.height / 4 + 5;
    for (const column of lines.vertical) {
      across(
        column.x,
        canvas.width,
        (x) => at(x, row),
        `column at ${String(column.x)}`,
      );
    }
    for (const line of lines.horizontal) {
      across(
        line.y,
        canvas.height,
        (y) => at(3, y),
        `row at ${String(line.y)}`,
      );
    }
  },
);
