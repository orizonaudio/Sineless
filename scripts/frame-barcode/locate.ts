// Finds the frame barcode's strips in a picture of the screen, and samples their cells: the
// recorder (record.swift) samples only the cells found here, so a minute of recording holds one
// grey value per cell per refresh rather than whole pictures.
import {
  barcodeRowCells,
  decodeRow,
  decodeStrip,
} from "../../ui/src/render/frame-barcode.ts";

// A picture of the screen, one grey value (0 black to 255 white) per pixel, rows from the top.
export interface GreyFrame {
  readonly width: number;
  readonly height: number;
  readonly data: Uint8Array;
}

// A strip's place in the recording, in its pixels from the top left.
export interface StripPlace {
  readonly left: number;
  readonly top: number;
  readonly cell: number;
}

// The two rows' greys, 0 to 1, at each cell's centre (or `at` of the way across and down it).
export function sampleStrip(
  frame: GreyFrame,
  place: StripPlace,
  at = 0.5,
): [number[], number[]] {
  const row = (index: number) =>
    Array.from({ length: barcodeRowCells }, (_, cell) => {
      const x = Math.floor(place.left + (cell + at) * place.cell);
      const y = Math.floor(place.top + (index + at) * place.cell);
      return x < frame.width && y < frame.height
        ? (frame.data[y * frame.width + x] ?? 0) / 255
        : 0;
    });
  return [row(0), row(1)];
}

// Every strip in `frames`, pictures of the screen taken while the traces move, each once (its top
// left), in reading order. A place counts only if it reads in every picture and its picture number
// changes between them: the screen's own dark background beside a lone white and black pixel can
// read as a row of zeros, but never advances. `cell`: the cells' side in the recording's pixels
// (the screen's device pixels, recorded at its own size).
export function locateStrips(
  frames: readonly GreyFrame[],
  cell: number,
): StripPlace[] {
  const [first, ...later] = frames;
  if (first === undefined || later.length === 0) {
    return [];
  }
  return candidates(first, cell).filter((place) => {
    const pictures = frames.map((frame) => {
      const read = decodeStrip(...sampleStrip(frame, place));
      return "picture" in read ? read.picture : undefined;
    });
    return (
      pictures.every((picture) => picture !== undefined) &&
      new Set(pictures).size > 1
    );
  });
}

// The places in one picture whose two rows read as rows.
function candidates(frame: GreyFrame, cell: number): StripPlace[] {
  const found: StripPlace[] = [];
  const width = barcodeRowCells * cell;
  const inside = (x: number, y: number) =>
    found.some(
      (place) =>
        x > place.left - width &&
        x < place.left + width &&
        y > place.top - 2 * cell &&
        y < place.top + 2 * cell,
    );
  const grey = (x: number, y: number) => frame.data[y * frame.width + x] ?? 0;
  const half = Math.floor(cell / 2);
  // A place reads when both rows read, and read alike a quarter and three quarters of the way
  // into each cell: a grid off the strip's by part of a cell straddles its cells, and so differs.
  const reads = (place: StripPlace) => {
    const readAt = (at: number) => {
      const [first, second] = sampleStrip(frame, place, at);
      const a = decodeRow(first);
      const b = decodeRow(second);
      return "value" in a && "value" in b
        ? `${String(a.value)},${String(b.value)}`
        : undefined;
    };
    const early = readAt(0.25);
    return early !== undefined && early === readAt(0.75);
  };
  for (let y = 0; y + 2 * cell <= frame.height; y++) {
    for (let x = 0; x + width <= frame.width; x++) {
      // The references first: white, then black, at the first row's centre.
      if (
        grey(x + half, y + half) < 160 ||
        grey(x + cell + half, y + half) > 96
      ) {
        continue;
      }
      if (inside(x, y)) {
        continue;
      }
      if (reads({ left: x, top: y, cell })) {
        // The first place that reads is at the edge of those that do: sample from the middle of
        // them, so each sample falls well inside its cell.
        let right = x;
        while (
          right + 1 < x + cell &&
          reads({ left: right + 1, top: y, cell })
        ) {
          right++;
        }
        const left = Math.floor((x + right) / 2);
        let bottom = y;
        while (
          bottom + 1 < y + cell &&
          reads({ left, top: bottom + 1, cell })
        ) {
          bottom++;
        }
        found.push({ left, top: Math.floor((y + bottom) / 2), cell });
      }
    }
  }
  return found;
}
