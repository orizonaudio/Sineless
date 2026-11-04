// The grid the page draws under its own traces (WebGL mode: Windows, and macOS before 14): the time
// divisions, each lane's level lines and its zero line, in the grid and axis colours, where the
// shared geometry puts them, and nothing left of a layout it no longer shows.
import { expect, test } from "vitest";

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
