// At the deepest view, late in a song, the musical grid still sits on its musical positions: the
// frame carries where the sweep starts precisely enough for a view of a few samples. Through the
// frame's own encoding, as the engine sends it.
import { describe, expect, test } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import { decodeFrame } from "../../protocol/src/frame-decoder.ts";
import { encodeFrame } from "../../protocol/src/frame-encoder.ts";
import { minimumViewSamples } from "../../protocol/src/limits.ts";
import { musicalGrid } from "../src/view/musical-grid.ts";

const rate = 48000;
const tempo = 120;
const span = 64; // Quarter notes: 16 bars of 4/4.
const spanSamples = (span * 60 * rate) / tempo;
const width = minimumViewSamples / spanSamples;

// Quarter note 16 is a control: exact in float32 too.
describe.each([16, 1200 + 1 / 3, 4800 + 0.1])(
  "a sweep starting at quarter note %s",
  (cycleStart) => {
    // The view at the floor, a beat a third of the way across it.
    const beat = Math.ceil(cycleStart + span / 2);
    const start = (beat - cycleStart) / span - width / 3;
    const view = { start, end: start + width };

    test("puts the beat line where the beat is", () => {
      const frame = decodeFrame(
        encodeFrame({
          mode: FrameMode.raw,
          triggered: false,
          frozen: true,
          freeRunning: false,
          synced: true,
          pointCount: 0,
          startFraction: 0,
          peak: 0,
          rms: 0,
          sampleRate: rate,
          cycleStart,
          cycleSeconds: (cycleStart * 60) / tempo,
          spanQuarterNotes: span,
          barQuarterNotes: 4,
          viewStart: view.start,
          viewEnd: view.end,
          visibleSamples: minimumViewSamples,
          views: new Map(),
        }),
      );
      const exact = musicalGrid(cycleStart, span, 4, view).lines;
      const carried = musicalGrid(
        frame.cycleStart,
        frame.spanQuarterNotes,
        frame.barQuarterNotes,
        { start: frame.viewStart, end: frame.viewEnd },
      ).lines;
      expect(exact.length).toBeGreaterThan(0);
      expect(carried.map((line) => line.position)).toHaveLength(exact.length);
      // The project time where the sweep starts, to well under a sample.
      expect(
        Math.abs(frame.cycleSeconds - (cycleStart * 60) / tempo),
      ).toBeLessThan(0.1 / rate);
      carried.forEach((line, index) => {
        // A thousandth of the screen: under a pixel on any screen (the bound, not a tuned value).
        expect(line.position).toBeCloseTo(
          exact[index]?.position ?? Number.NaN,
          3,
        );
      });
    });
  },
);

// A sweep's playhead near the end of a long sweep (16 bars at 60 BPM and 96 kHz) arrives where it
// was sent, to well under a sample: zoomed in to a few samples, it is drawn on the join between
// the passes.
test("carries the playhead to the sample", () => {
  const sweepSamples = 6_144_000;
  const cursor = 1 - 5 / sweepSamples;
  const frame = decodeFrame(
    encodeFrame({
      mode: FrameMode.raw,
      triggered: false,
      frozen: true,
      freeRunning: false,
      synced: true,
      pointCount: 0,
      startFraction: 0,
      peak: 0,
      rms: 0,
      sampleRate: rate,
      cursor,
      views: new Map(),
    }),
  );
  expect(
    Math.abs((frame.cursor ?? Number.NaN) - cursor) * sweepSamples,
  ).toBeLessThan(1e-3);
});
