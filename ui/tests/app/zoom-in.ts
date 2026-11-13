// Zooms in at the screen's centre, ⌘ and wheel as a user does, until the view stops narrowing:
// the narrowest view (minimumViewSamples of the span), the same in WebKit and Chromium.
import { expect } from "vitest";

import { minimumViewSamples } from "../../../protocol/src/limits.ts";
import { byId, settle } from "./harness.ts";

export async function zoomToNarrowest(): Promise<void> {
  const trace = byId("trace");
  const bounds = trace.getBoundingClientRect();
  const samples = () =>
    window.orizonDebug?.lastFrame?.visibleSamples ?? Number.POSITIVE_INFINITY;
  for (
    let step = 0;
    step < 80 && samples() > minimumViewSamples * 1.001;
    step++
  ) {
    trace.dispatchEvent(
      new WheelEvent("wheel", {
        deltaY: -400,
        metaKey: true,
        clientX: bounds.left + bounds.width / 2,
        clientY: bounds.top + bounds.height / 2,
        bubbles: true,
        cancelable: true,
      }),
    );
    await settle(1);
  }
  expect(samples()).toBeCloseTo(minimumViewSamples, 2);
  await settle();
}
