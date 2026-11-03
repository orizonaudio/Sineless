// The frame barcode's rows (protocol/src/limits.ts, barcodeDataBits): what the page draws while the
// diagnostic switch is on, and what the macOS editor's encoder must match
// (protocol/tests/data/frame-barcode.json).
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
