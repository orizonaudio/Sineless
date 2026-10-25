import { describe, expect, test } from "vitest";

import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import {
  clampPan,
  markerLane,
  readoutView,
  viewOrder,
  viewToggleState,
} from "../src/view/screen-state.ts";

describe.each(viewOrder)("triggering on %s", (source) => {
  test("Seconds mode measures the trigger source", () => {
    expect(readoutView(false, source, 2)).toBe(ChannelView[source]);
  });
  test("Beats mode measures the Mid, as no trigger applies", () => {
    expect(readoutView(true, source, 2)).toBe(ChannelView.mid);
  });
  test("a mono track measures L, in either mode", () => {
    expect(readoutView(false, source, 1)).toBe(ChannelView.left);
    expect(readoutView(true, source, 1)).toBe(ChannelView.left);
  });
});

describe("clampPan", () => {
  test.each([
    [-1, 4, 0],
    [0, 4, 0],
    [2.5, 4, 2.5],
    [4, 4, 4],
    [6, 4, 4],
    [3, 1, 1], // The span grew: the reach shrank under the pan.
    [3, 0, 0],
  ])("%s s back with %s s held pans %s s", (seconds, reach, panned) => {
    expect(clampPan(seconds, reach)).toBe(panned);
  });
});

describe("markerLane", () => {
  test("overlaid, the one lane whatever the source", () => {
    for (const source of viewOrder) {
      expect(markerLane(ViewLayout.overlaid, false, 2, viewOrder, source)).toBe(
        0,
      );
      expect(markerLane(ViewLayout.overlaid, true, 2, viewOrder, source)).toBe(
        0,
      );
    }
  });
  test.each(viewOrder)("stacked, the lane of %s when drawn", (source) => {
    expect(markerLane(ViewLayout.stacked, false, 0, viewOrder, source)).toBe(
      viewOrder.indexOf(source),
    );
  });
  test.each(viewOrder)(
    "stacked on a mono track, the first lane for %s",
    (source) => {
      expect(markerLane(ViewLayout.stacked, false, 0, ["left"], source)).toBe(
        0,
      );
    },
  );
  test.each([0, 1, 3])(
    "stacked tracks, the own track's lane %i whatever the source",
    (own) => {
      for (const source of viewOrder) {
        expect(markerLane(ViewLayout.stacked, true, own, ["mid"], source)).toBe(
          own,
        );
      }
    },
  );
  test("stacked tracks without the own lane found, the first", () => {
    expect(markerLane(ViewLayout.stacked, true, -1, ["mid"], "left")).toBe(0);
  });
});

describe.each(viewOrder)("the %s toggle", (view) => {
  test("is pressed when shown, and only then", () => {
    expect(viewToggleState(view, [view], false, 2).pressed).toBe(true);
    expect(
      viewToggleState(
        view,
        viewOrder.filter((other) => other !== view),
        false,
        2,
      ).pressed,
    ).toBe(false);
  });
  test("is disabled in the multi-track view", () => {
    expect(viewToggleState(view, [view], true, 2).disabled).toBe(true);
  });
  test("on a mono track, is enabled only for Left", () => {
    expect(viewToggleState(view, [view], false, 1).disabled).toBe(
      view !== "left",
    );
  });
  test("on a stereo track, is enabled", () => {
    expect(viewToggleState(view, [view], false, 2).disabled).toBe(false);
  });
});
