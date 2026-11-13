// Zoom reaches the same number of samples at every span: from the shortest Free span to the
// longest, the narrowest view shows minimumViewSamples, and the frame carries the wave between
// them (more than one point a sample), whether the user zooms in with ⌘ and the wheel, drags
// across a region narrower than that, or pinches on a trackpad.
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

function samplesShown(): number {
  return (
    window.orizonDebug?.lastFrame?.visibleSamples ?? Number.POSITIVE_INFINITY
  );
}

// A pointer event on the trace at `x`, a fraction of the screen's width, mid-height.
function screenPointer(type: string, x: number): void {
  const box = byId("screen").getBoundingClientRect();
  byId("trace").dispatchEvent(
    new PointerEvent(type, {
      pointerId: 1,
      bubbles: true,
      button: 0,
      buttons: type === "pointerup" ? 0 : 1,
      clientX: box.left + x * box.width,
      clientY: box.top + box.height / 2,
    }),
  );
}

// A WebKit gesture event at the screen's centre, at `scale` of its start.
function gesture(type: string, scale: number): void {
  const box = byId("screen").getBoundingClientRect();
  const event = new Event(type, { bubbles: true, cancelable: true });
  Object.defineProperties(event, {
    scale: { value: scale },
    clientX: { value: box.left + box.width / 2 },
    clientY: { value: box.top + box.height / 2 },
  });
  byId("screen").dispatchEvent(event);
}

describe.each([settings.freeSpan.minimum, settings.freeSpan.maximum])(
  "a %s s free-running span",
  (seconds) => {
    const start = async () => {
      await startApp();
      await inTimeBase("Free");
      await typeInto("free-span-field", formatSeconds(seconds));
      await settle();
    };
    const expectFloor = () => {
      const frame = window.orizonDebug?.lastFrame;
      expect(frame?.visibleSamples).toBeCloseTo(minimumViewSamples, 2);
      expect(frame?.pointsPerSample ?? 0).toBeGreaterThan(1);
    };

    test("zooms in to the fewest samples and draws the wave between them", async () => {
      await start();
      await zoomToNarrowest();
      expectFloor();
    });

    // Each drag across a region of a few pixels: narrower than the fewest samples by the last.
    test("stops a drag across a narrower region at the fewest samples", async () => {
      await start();
      for (
        let drag = 0;
        drag < 8 && samplesShown() > minimumViewSamples * 1.001;
        drag++
      ) {
        screenPointer("pointerdown", 0.5);
        screenPointer("pointermove", 0.505);
        screenPointer("pointerup", 0.505);
        await settle(1);
      }
      await settle();
      expectFloor();
    });

    // A trackpad pinch, as WebKit reports it: one gesture spreading far past the floor.
    test("stops a pinch at the fewest samples", async () => {
      await start();
      gesture("gesturestart", 1);
      for (let scale = 1; scale < 1e7; scale *= 1.5) {
        gesture("gesturechange", scale);
        await settle(1);
      }
      gesture("gestureend", 1e7);
      await settle();
      expectFloor();
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
