// The frame barcode's rows (the debug switch's strip, read back from screen recordings): what a
// decoder relies on, and the shared cases the macOS editor's encoder is checked against
// (protocol/tests/data/frame-barcode.json). After an intended change, write the file again with
// ORIZON_UPDATE_VECTORS=1 and review the diff.
import { readFileSync, writeFileSync } from "node:fs";
import { expect, test } from "vitest";

import { barcodeDataBits } from "../../protocol/src/limits.ts";
import { barcodeRow, barcodeRowCells } from "../src/render/frame-barcode.ts";

const file = new URL(
  "../../protocol/tests/data/frame-barcode.json",
  import.meta.url,
);
const range = 2 ** barcodeDataBits;

// Every single bit, the ends of the range, the wrap and a few others.
const values = [
  0,
  1,
  ...Array.from({ length: barcodeDataBits }, (_, bit) => 2 ** bit),
  range - 1,
  range,
  range + 5,
  12345,
  43690,
];

test.each(values)(
  "row for %i: references, the value most significant first, even parity, a black end",
  (value) => {
    const cells = barcodeRow(value);
    expect(cells).toHaveLength(barcodeRowCells);
    expect(cells[0]).toBe(true);
    expect(cells[1]).toBe(false);
    expect(cells.at(-1)).toBe(false);
    const data = cells.slice(2, 2 + barcodeDataBits);
    const read = data.reduce((sum, white) => sum * 2 + (white ? 1 : 0), 0);
    expect(read).toBe(value % range);
    const whites = cells.slice(2, -1).filter(Boolean).length;
    expect(whites % 2).toBe(0);
  },
);

test("a value one bit away from another differs in two cells: a single misread is caught", () => {
  for (let bit = 0; bit < barcodeDataBits; bit++) {
    const a = barcodeRow(1234);
    const b = barcodeRow(1234 ^ (1 << bit));
    expect(a.filter((cell, index) => cell !== b[index])).toHaveLength(2);
  }
});

test("the editor's rows are the page's", () => {
  const vectors = {
    description:
      "Frame barcode rows, from the page's encoder (ui/tests/frame-barcode.test.ts), read by the editor's C++ tests: 1 is a white cell. Write again with ORIZON_UPDATE_VECTORS=1.",
    rows: values.map((value) => ({
      value,
      cells: barcodeRow(value).map((white) => (white ? 1 : 0)),
    })),
  };
  if (process.env["ORIZON_UPDATE_VECTORS"] === "1") {
    writeFileSync(file, `${JSON.stringify(vectors, null, 2)}\n`);
  }
  const kept: unknown = JSON.parse(readFileSync(file, "utf8"));
  expect(JSON.parse(JSON.stringify(vectors))).toEqual(kept);
});
