// The analysis command's two inputs: record.sh's recording (one grey per cell per refresh) and a
// movie, each read into the counts. The movie case encodes one with ffmpeg where it is installed.
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { expect, test } from "vitest";

import {
  barcodeCellPixels,
  barcodeTicksPerMillisecond,
} from "../../protocol/src/limits.ts";
import { barcodeRow } from "../../ui/src/render/frame-barcode.ts";
import {
  formatCounts,
  movieSamples,
  parseRecording,
} from "../frame-barcode/analyse.ts";
import { countRefreshes } from "../frame-barcode/stats.ts";

const period = 1000 / 60;
const greys = (value: number) =>
  barcodeRow(value).map((white) => (white ? 0.9 : 0.05));

test("a recording reads into counts: a strip stepping every refresh, an idle refresh, a gap", () => {
  const lines = [
    JSON.stringify({ display: 1, strips: [{ left: 0, top: 0, cell: 4 }] }),
  ];
  for (let refresh = 0; refresh < 300; refresh++) {
    if (refresh === 150) {
      continue; // The recorder missed it.
    }
    const t = 5000 + refresh * period;
    if (refresh === 200) {
      lines.push(JSON.stringify({ t, idle: true }));
      continue;
    }
    const picture = refresh < 200 ? refresh : refresh - 1;
    const drawnFor =
      Math.round((t - 33.3) * barcodeTicksPerMillisecond) % 65536;
    lines.push(
      JSON.stringify({ t, cells: [[greys(picture), greys(drawnFor)]] }),
    );
  }
  const counts = countRefreshes(parseRecording(lines.join("\n")));
  expect(counts.periodMs).toBeCloseTo(period, 3);
  expect(counts.strips[0]).toMatchObject({
    unknownAcrossGaps: 1,
    idle: 1,
    lost: 0,
  });
  const table = formatCounts(counts);
  expect(table).toContain("Strip 1:");
  expect(table).toContain("unknown, recorder gaps");
});

const ffmpeg = spawnSync("ffmpeg", ["-version"]).status === 0;

test.runIf(ffmpeg)(
  "a movie of the screen reads into counts as a recording does",
  async () => {
    const cell = barcodeCellPixels;
    const width = 320;
    const height = 120;
    const frames = 90;
    const raw = new Uint8Array(width * height * frames).fill(10);
    for (let frame = 0; frame < frames; frame++) {
      const drawnFor = Math.round(frame * period * barcodeTicksPerMillisecond);
      [barcodeRow(frame), barcodeRow(drawnFor)].forEach((cells, row) => {
        cells.forEach((white, index) => {
          for (let y = 0; y < cell; y++) {
            for (let x = 0; x < cell; x++) {
              raw[
                frame * width * height +
                  (20 + row * cell + y) * width +
                  200 +
                  index * cell +
                  x
              ] = white ? 240 : 8;
            }
          }
        });
      });
    }
    const dir = mkdtempSync(join(tmpdir(), "frame-barcode-"));
    const rawFile = join(dir, "frames.gray");
    const movie = join(dir, "screen.mov");
    writeFileSync(rawFile, raw);
    const encode = spawnSync("ffmpeg", [
      "-v",
      "error",
      "-f",
      "rawvideo",
      "-pix_fmt",
      "gray",
      "-s",
      `${String(width)}x${String(height)}`,
      "-r",
      "60",
      "-i",
      rawFile,
      "-c:v",
      "ffv1",
      movie,
    ]);
    expect(encode.status).toBe(0);
    const counts = countRefreshes(await movieSamples(movie, cell));
    expect(counts.strips).toHaveLength(1);
    expect(counts.strips[0]).toMatchObject({
      lost: 0,
      unreadable: 0,
      unknownAcrossGaps: 0,
    });
    expect(counts.strips[0]?.newOnTime).toBe(frames - 1);
  },
);
