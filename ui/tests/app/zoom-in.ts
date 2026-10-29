// Zooms in at the screen's centre, ⌘ and wheel as a user does, until the view stops narrowing:
// the narrowest view (minimumViewWidth), the same in WebKit and Chromium.
import { expect } from "vitest";

import { minimumViewWidth } from "../../../protocol/src/limits.ts";
import { byId, settle } from "./harness.ts";

export async function zoomToNarrowest(): Promise<void> {
  const trace = byId("trace");
  const bounds = trace.getBoundingClientRect();
  const width = () => {
    const frame = window.orizonDebug?.lastFrame;
    return frame === undefined ? 1 : frame.viewEnd - frame.viewStart;
  };
  for (let step = 0; step < 60 && width() > minimumViewWidth * 1.001; step++) {
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
  expect(width()).toBeCloseTo(minimumViewWidth, 6);
  await settle();
}
