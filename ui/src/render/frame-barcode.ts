// The frame barcode's rows (protocol/src/limits.ts, barcodeDataBits): what the page draws while the
// diagnostic switch is on, what the macOS editor's encoder must match
// (protocol/tests/data/frame-barcode.json), and how a row is read back from a recording: from one
// grey value per cell, sampled at each cell's centre, to the value it carries or why it cannot be
// read. A row is never guessed: anything short of two clear references, cells clearly white or
// black, a black end and even parity is unreadable.
import { barcodeDataBits } from "../../../protocol/src/limits.ts";

// A white and a black reference, the value's cells, parity and the end.
export const barcodeRowCells = barcodeDataBits + 4;

// The row for `value`, modulo the range: true is a white cell.
export function barcodeRow(value: number): boolean[] {
  const range = 2 ** barcodeDataBits;
  const wrapped = ((Math.trunc(value) % range) + range) % range;
  const data = Array.from(
    { length: barcodeDataBits },
    (_, index) =>
      Math.floor(wrapped / 2 ** (barcodeDataBits - 1 - index)) % 2 === 1,
  );
  const odd = data.filter(Boolean).length % 2 === 1;
  return [true, false, ...data, odd, false];
}

export type RowRead =
  { readonly value: number } | { readonly unreadable: string };

export type StripRead =
  | { readonly picture: number; readonly drawnFor: number }
  | { readonly unreadable: string };

// The references must differ by at least this share of the full grey range (0 to 1): less is no
// strip at all (covered, or never drawn).
const minimumContrast = 0.25;
// A cell must lie this share of the contrast away from the threshold: one in between is crossed by
// something (a cursor line, the hover line) or blurred, and is not read.
const cellMargin = 0.25;

// `greys`: one value per cell, 0 black to 1 white, in the row's order.
export function decodeRow(greys: readonly number[]): RowRead {
  if (greys.length !== barcodeRowCells) {
    return { unreadable: "cell count" };
  }
  const white = greys[0] ?? 0;
  const black = greys[1] ?? 0;
  const contrast = white - black;
  if (contrast < minimumContrast) {
    return { unreadable: "references" };
  }
  const threshold = (white + black) / 2;
  const cells: boolean[] = [];
  for (const grey of greys.slice(2)) {
    if (Math.abs(grey - threshold) < cellMargin * contrast) {
      return { unreadable: "cell neither white nor black" };
    }
    cells.push(grey > threshold);
  }
  if (cells.at(-1) === true) {
    return { unreadable: "end" };
  }
  const data = cells.slice(0, barcodeDataBits);
  const parity = cells[barcodeDataBits] === true;
  if ((data.filter(Boolean).length + (parity ? 1 : 0)) % 2 !== 0) {
    return { unreadable: "parity" };
  }
  return { value: data.reduce((sum, bit) => sum * 2 + (bit ? 1 : 0), 0) };
}

// A strip's two rows: the picture's number and the time it was drawn for.
export function decodeStrip(
  picture: readonly number[],
  drawnFor: readonly number[],
): StripRead {
  const first = decodeRow(picture);
  if ("unreadable" in first) {
    return { unreadable: `picture: ${first.unreadable}` };
  }
  const second = decodeRow(drawnFor);
  if ("unreadable" in second) {
    return { unreadable: `drawn for: ${second.unreadable}` };
  }
  return { picture: first.value, drawnFor: second.value };
}
