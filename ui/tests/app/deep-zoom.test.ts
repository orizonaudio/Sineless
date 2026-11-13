// Zoom reaches the same number of samples at every span: from the shortest Free span to the
// longest, the narrowest view shows minimumViewSamples, and the frame carries the wave between
// them (more than one point a sample), as the user zooms in with ⌘ and the wheel.
import { describe, expect, test } from "vitest";

import { minimumViewSamples } from "../../../protocol/src/limits.ts";
import { settings } from "../../../protocol/src/settings.ts";
import { formatSeconds } from "../../src/view/format.ts";
import { inTimeBase, settle, startApp, typeInto } from "./harness.ts";
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
