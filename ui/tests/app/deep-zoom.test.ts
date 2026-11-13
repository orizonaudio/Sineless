// Zoom reaches the same number of samples at every span: from the shortest Free span to the
// longest, the narrowest view shows minimumViewSamples, and the frame carries the wave between
// them (more than one point a sample), as the user zooms in with ⌘ and the wheel.
import { describe, expect, test } from "vitest";

import { minimumViewSamples } from "../../../protocol/src/limits.ts";
import { settings } from "../../../protocol/src/settings.ts";
import { formatSeconds } from "../../src/view/format.ts";
import {
  byId,
  inTimeBase,
  settle,
  startApp,
  startBeforeFrames,
  typeInto,
} from "./harness.ts";
import { zoomToNarrowest } from "./zoom-in.ts";

describe.each([settings.freeSpan.minimum, settings.freeSpan.maximum])(
  "a %s s free-running span",
  (seconds) => {
    test("zooms in to the fewest samples and draws the wave between them", async () => {
      await startApp();
      await inTimeBase("Free");
      await typeInto("free-span-field", formatSeconds(seconds));
      await settle();
      await zoomToNarrowest();
      const frame = window.orizonDebug?.lastFrame;
      expect(frame?.visibleSamples).toBeCloseTo(minimumViewSamples, 2);
      expect(frame?.pointsPerSample ?? 0).toBeGreaterThan(1);
    });
  },
);

// Before the first frame (a Normal trigger waiting for its first trigger), the span's samples are
// not known: a drag across the screen ends as drags do, its rectangle gone, and zooms nothing
// (the view stays the whole span).
test("a drag before the first frame clears its selection and zooms nothing", async () => {
  const app = await startBeforeFrames();
  const trace = byId("trace");
  const box = byId("screen").getBoundingClientRect();
  const at = (type: string, x: number) => {
    trace.dispatchEvent(
      new PointerEvent(type, {
        pointerId: 1,
        bubbles: true,
        button: 0,
        buttons: type === "pointerup" ? 0 : 1,
        clientX: box.left + x * box.width,
        clientY: box.top + box.height / 2,
      }),
    );
  };
  const before = app.sent.length;
  at("pointerdown", 0.2);
  at("pointermove", 0.6);
  expect(byId("selection").hidden).toBe(false);
  at("pointerup", 0.6);
  expect(byId("selection").hidden).toBe(true);
  // Any view sent is the whole span.
  for (const message of app.sent.slice(before)) {
    if (message.type === "view") {
      expect([message.start, message.end]).toEqual([0, 1]);
    }
  }
});
