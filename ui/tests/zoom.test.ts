import { describe, expect, test } from "vitest";

import { minimumViewSamples } from "../../protocol/src/limits.ts";
import { settings } from "../../protocol/src/settings.ts";
import {
  fullView,
  isZoomed,
  minimumWidth,
  panBy,
  toFrame,
  toScreen,
  zoomAround,
  zoomFactor,
  zoomTo,
  type ViewWindow,
} from "../src/view/zoom.ts";

// A span long enough not to limit the tests that are not about the limit.
const long = minimumWidth(1e9);

describe("view zoom", () => {
  test("starts at the whole frame", () => {
    expect(isZoomed(fullView)).toBe(false);
    expect(zoomFactor(fullView)).toBe(1);
  });

  test("keeps the point under the pointer fixed while zooming", () => {
    const view = zoomAround(fullView, 0.25, 4, long);
    expect(zoomFactor(view)).toBeCloseTo(4);
    expect(toFrame(view, 0.25)).toBeCloseTo(0.25);
  });

  test("never shows beyond the frame or zooms out past it", () => {
    const zoomedNearEnd = zoomAround(fullView, 1, 2, long);
    expect(zoomedNearEnd.end).toBe(1);
    expect(zoomAround(zoomedNearEnd, 0.5, 0.1, long)).toEqual(fullView);
  });

  test("zooms exactly to a dragged region, in either direction", () => {
    const view = zoomTo(fullView, 0.6, 0.2, long);
    expect(view.start).toBeCloseTo(0.2);
    expect(view.end).toBeCloseTo(0.6);
    // A second selection is relative to the current view.
    const inner = zoomTo(view, 0.5, 1, long);
    expect(inner.start).toBeCloseTo(0.4);
    expect(inner.end).toBeCloseTo(0.6);
  });

  test("pans by screen widths and stops at the frame's edges", () => {
    const view = zoomTo(fullView, 0.4, 0.6, long);
    const moved = panBy(view, 0.5);
    expect(moved.start).toBeCloseTo(0.5);
    expect(panBy(view, -10).start).toBe(0);
  });

  test("maps between screen and frame positions", () => {
    const view = zoomTo(fullView, 0.2, 0.6, long);
    expect(toScreen(view, 0.4)).toBeCloseTo(0.5);
    expect(toFrame(view, 0.5)).toBeCloseTo(0.4);
  });
});

// The spans a view can show, from the shortest Trig span to 16 bars at 60 BPM, at the rates hosts
// use: zoom reaches minimumViewSamples of each, the same at every span, and no further.
const rates = [44100, 48000, 96000, 192000, 384000];
const spanSeconds = [
  settings.triggerSpan.minimum,
  settings.freeSpan.minimum,
  1,
  settings.freeSpan.maximum,
  64, // 16 bars at 60 BPM.
];
const spans = rates.flatMap((rate) =>
  spanSeconds.map((seconds) => ({ rate, seconds, samples: seconds * rate })),
);

function samplesShown(view: ViewWindow, spanSamples: number): number {
  return (view.end - view.start) * spanSamples;
}

describe.each(spans)(
  "zoom on $seconds s at $rate Hz",
  ({ samples: spanSamples }) => {
    const minimum = minimumWidth(spanSamples);
    const zoomable = spanSamples > minimumViewSamples;

    test("the wheel and pinch stop at the fewest samples, wherever the pointer is", () => {
      for (const anchor of [0, 0.37, 1]) {
        let view = fullView;
        for (let step = 0; step < 100; step++) {
          view = zoomAround(view, anchor, 1.3, minimum);
        }
        expect(samplesShown(view, spanSamples)).toBeCloseTo(
          zoomable ? minimumViewSamples : spanSamples,
          6,
        );
        expect(view.start).toBeGreaterThanOrEqual(0);
        expect(view.end).toBeLessThanOrEqual(1);
      }
    });

    test("a drag across a narrower region shows the fewest samples", () => {
      const view = zoomTo(fullView, 0.5, 0.5 + 1e-9, minimum);
      expect(samplesShown(view, spanSamples)).toBeCloseTo(
        zoomable ? minimumViewSamples : spanSamples,
        6,
      );
      expect(isZoomed(view)).toBe(zoomable);
    });
  },
);

test("a span not known yet (before the first frame) is not zoomed", () => {
  expect(zoomAround(fullView, 0.5, 4, minimumWidth(0))).toEqual(fullView);
});
