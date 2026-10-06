import { describe, expect, test } from "vitest";

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
} from "../src/view/zoom.ts";

describe("view zoom", () => {
  test("starts at the whole frame", () => {
    expect(isZoomed(fullView)).toBe(false);
    expect(zoomFactor(fullView)).toBe(1);
  });

  test("keeps the point under the pointer fixed while zooming", () => {
    const view = zoomAround(fullView, 0.25, 4);
    expect(zoomFactor(view)).toBeCloseTo(4);
    expect(toFrame(view, 0.25)).toBeCloseTo(0.25);
  });

  test("never shows beyond the frame or zooms out past it", () => {
    const zoomedNearEnd = zoomAround(fullView, 1, 2);
    expect(zoomedNearEnd.end).toBe(1);
    expect(zoomAround(zoomedNearEnd, 0.5, 0.1)).toEqual(fullView);
  });

  test("limits magnification", () => {
    expect(zoomFactor(zoomAround(fullView, 0.5, 10_000))).toBeCloseTo(
      1 / minimumWidth,
    );
  });

  test("zooms exactly to a dragged region, in either direction", () => {
    const view = zoomTo(fullView, 0.6, 0.2);
    expect(view.start).toBeCloseTo(0.2);
    expect(view.end).toBeCloseTo(0.6);
    // A second selection is relative to the current view.
    const inner = zoomTo(view, 0.5, 1);
    expect(inner.start).toBeCloseTo(0.4);
    expect(inner.end).toBeCloseTo(0.6);
  });

  test("pans by screen widths and stops at the frame's edges", () => {
    const view = zoomTo(fullView, 0.4, 0.6);
    const moved = panBy(view, 0.5);
    expect(moved.start).toBeCloseTo(0.5);
    expect(panBy(view, -10).start).toBe(0);
  });

  test("maps between screen and frame positions", () => {
    const view = zoomTo(fullView, 0.2, 0.6);
    expect(toScreen(view, 0.4)).toBeCloseTo(0.5);
    expect(toFrame(view, 0.5)).toBeCloseTo(0.4);
  });
});
