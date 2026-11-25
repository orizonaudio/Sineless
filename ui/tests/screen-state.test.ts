import { describe, expect, test } from "vitest";

import { ChannelView, ViewLayout } from "../../protocol/src/enums.ts";
import {
  clampPan,
  markerLane,
  meterSource,
  viewNames,
  viewOrder,
  viewToggleState,
} from "../src/view/screen-state.ts";

// The Meter's Source names the channel the frame's levels were measured on (#393, #420).
describe("meterSource", () => {
  test.each(Object.values(ChannelView))(
    "names channel view %i in Single",
    (view) => {
      expect(meterSource({ laneSet: 0, readoutView: view })).toBe(
        viewNames[view],
      );
    },
  );

  test.each(Object.values(ChannelView))(
    "names this track and its lane channel %i for a frame with lanes",
    (view) => {
      expect(meterSource({ laneSet: 3, readoutView: view })).toBe(
        `This track (${viewNames[view]})`,
      );
    },
  );

  test("has no value before any frame", () => {
    expect(meterSource(undefined)).toBe("—");
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
    expect(viewToggleState(view, [view], 2).pressed).toBe(true);
    expect(
      viewToggleState(
        view,
        viewOrder.filter((other) => other !== view),
        2,
      ).pressed,
    ).toBe(false);
  });
  test("on a mono track, is enabled only for Left", () => {
    expect(viewToggleState(view, [view], 1).disabled).toBe(view !== "left");
  });
  test("on a stereo track, is enabled", () => {
    expect(viewToggleState(view, [view], 2).disabled).toBe(false);
  });
});
