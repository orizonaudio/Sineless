// Finding the frame barcode's strips in pictures of the screen, and sampling their cells, on
// pictures drawn here: a strip at the page's own cell size and at half of it (a recording made at
// the screen's points, not its pixels), noise, two strips, look-alikes, a strip that does not
// advance, and a strip that moved. Each case takes two pictures, the strip a picture on.
import { describe, expect, test } from "vitest";

import { barcodeCellPixels } from "../../protocol/src/limits.ts";
import {
  barcodeRow,
  barcodeRowCells,
  decodeStrip,
} from "../../ui/src/render/frame-barcode.ts";
import {
  locateStrips,
  sampleStrip,
  type GreyFrame,
} from "../frame-barcode/locate.ts";

function blankFrame(width: number, height: number, grey = 20): GreyFrame {
  return { width, height, data: new Uint8Array(width * height).fill(grey) };
}

// Draws a strip's two rows at (left, top), in cells of `cell` pixels.
function drawStrip(
  frame: GreyFrame,
  left: number,
  top: number,
  cell: number,
  picture: number,
  drawnFor: number,
): void {
  [barcodeRow(picture), barcodeRow(drawnFor)].forEach((cells, row) => {
    cells.forEach((white, index) => {
      for (let y = 0; y < cell; y++) {
        for (let x = 0; x < cell; x++) {
          const at =
            (top + row * cell + y) * frame.width + left + index * cell + x;
          frame.data[at] = white ? 235 : 12;
        }
      }
    });
  });
}

describe("finding strips", () => {
  test.each([barcodeCellPixels, barcodeCellPixels / 2])(
    "finds a strip of %i-pixel cells and reads it back",
    (cell) => {
      const frame = blankFrame(400, 200);
      drawStrip(frame, 301, 13, cell, 4321, 60000);
      const next = blankFrame(400, 200);
      drawStrip(next, 301, 13, cell, 4322, 60067);
      const places = locateStrips([frame, next], cell);
      expect(places).toHaveLength(1);
      const [first, second] = sampleStrip(
        frame,
        places[0] ?? { left: 0, top: 0, cell },
      );
      expect(decodeStrip(first, second)).toEqual({
        picture: 4321,
        drawnFor: 60000,
      });
    },
  );

  test("samples each cell well inside it", () => {
    const cell = barcodeCellPixels;
    const frame = blankFrame(400, 200);
    drawStrip(frame, 120, 40, cell, 7, 8);
    const next = blankFrame(400, 200);
    drawStrip(next, 120, 40, cell, 8, 75);
    const [place] = locateStrips([frame, next], cell);
    expect(place?.left).toBeGreaterThanOrEqual(120 - 1);
    expect(place?.left).toBeLessThanOrEqual(120 + 1);
    expect(place?.top).toBeGreaterThanOrEqual(40 - 1);
    expect(place?.top).toBeLessThanOrEqual(40 + 1);
  });

  test("finds two strips, once each, through noise", () => {
    const cell = barcodeCellPixels;
    const frame = blankFrame(800, 300);
    drawStrip(frame, 100, 20, cell, 1, 2);
    drawStrip(frame, 600, 200, cell, 3, 4);
    const next = blankFrame(800, 300);
    drawStrip(next, 100, 20, cell, 2, 69);
    drawStrip(next, 600, 200, cell, 4, 71);
    frame.data.forEach((grey, index) => {
      frame.data[index] = Math.max(
        0,
        Math.min(255, grey + ((index * 7919) % 17) - 8),
      );
    });
    const places = locateStrips([frame, next], cell);
    expect(places).toHaveLength(2);
    expect(
      places.map((place) => decodeStrip(...sampleStrip(frame, place))),
    ).toEqual([
      { picture: 1, drawnFor: 2 },
      { picture: 3, drawnFor: 4 },
    ]);
  });

  test("finds nothing in a look-alike: references alone, or a row failing parity", () => {
    const cell = barcodeCellPixels;
    const frame = blankFrame(400, 200);
    drawStrip(frame, 50, 50, cell, 1234, 5678);
    // One cell of the second row flipped: its parity fails.
    for (let y = 0; y < cell; y++) {
      for (let x = 0; x < cell; x++) {
        const at = (50 + cell + y) * frame.width + 50 + 9 * cell + x;
        frame.data[at] = (frame.data[at] ?? 0) > 128 ? 12 : 235;
      }
    }
    const next = blankFrame(400, 200);
    next.data.set(frame.data);
    expect(locateStrips([frame, next], cell)).toEqual([]);
  });

  test("finds nothing in a strip that does not advance: a picture left on screen", () => {
    const cell = barcodeCellPixels;
    const frame = blankFrame(400, 200);
    drawStrip(frame, 60, 60, cell, 99, 1000);
    expect(locateStrips([frame, frame], cell)).toEqual([]);
    expect(locateStrips([frame], cell)).toEqual([]);
  });

  test("a strip that moved reads unreadable from its old place, never as other values", () => {
    const cell = barcodeCellPixels;
    const before = blankFrame(400, 200);
    drawStrip(before, 100, 30, cell, 10, 20);
    const next = blankFrame(400, 200);
    drawStrip(next, 100, 30, cell, 11, 87);
    const [place] = locateStrips([before, next], cell);
    const after = blankFrame(400, 200);
    drawStrip(after, 100 + 2 * cell + 1, 30, cell, 11, 21);
    const read = decodeStrip(
      ...sampleStrip(after, place ?? { left: 0, top: 0, cell }),
    );
    expect("unreadable" in read).toBe(true);
    expect(barcodeRowCells).toBeGreaterThan(2);
  });
});
