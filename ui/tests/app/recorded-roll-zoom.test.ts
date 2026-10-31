// A live 1.5 s free-running window zoomed to the narrowest view, on the real engine's frames: the
// engine reads only the view, so the frame carries a few hundred samples of the span, and the axis
// still marks the span shown.
import { beforeAll, describe, expect, test } from "vitest";

import { timeLabels as axisTimeLabels } from "../../src/view/axis-labels.ts";
import { parseQuantity } from "../../src/view/format.ts";
import { type ViewWindow } from "../../src/view/zoom.ts";
import { byId, settle, startRecorded } from "./harness.ts";
import { zoomToNarrowest } from "./zoom-in.ts";

const span = 1.5;

function labelSeconds(): number[] {
  return [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.firstChild?.textContent ?? "")
    .filter((text) => text.endsWith("s"))
    .map((text) => parseQuantity(text, "time") ?? Number.NaN);
}

// Whether the labels mark a rolling span of `span` seconds seen through `view`, within the
// labels' three digits.
function marksSpan(view: ViewWindow): boolean {
  const expected = axisTimeLabels(span, true, view).map((label) =>
    parseQuantity(label.text, "time"),
  );
  const shown = labelSeconds();
  return (
    shown.length === expected.length &&
    shown.every(
      (seconds, index) =>
        Math.abs(seconds - (expected[index] ?? 0)) <= 0.01 * span,
    )
  );
}

describe("a recorded live free-running window zoomed to the narrowest view", () => {
  beforeAll(async () => {
    await startRecorded("Roll1500msDeepestZoom_48k");
  });

  test("keeps the axis to the span shown", async () => {
    await settle();
    await zoomToNarrowest();
    const frame = window.orizonDebug?.lastFrame;
    const view = { start: frame?.viewStart ?? 0, end: frame?.viewEnd ?? 1 };
    // The frame carries the view's samples, not the span's.
    expect(frame?.visibleSamples ?? 0).toBeLessThan(span * 48000 * 0.01);
    await expect
      .poll(() => marksSpan(view), { message: labelSeconds().join(", ") })
      .toBe(true);
  });
});
