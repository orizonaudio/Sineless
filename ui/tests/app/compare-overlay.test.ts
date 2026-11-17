// Multi with the tracks overlaid and Compare on, through the real page: Compare's lane, the sum or
// the difference, is a lane of its own below the tracks' (FR-15.1), never drawn over or under
// them. Everything placed in lanes follows: the level labels, the lane labels and Compare's
// readouts, the hover, both kinds of cursor and their snapping, and the trigger marker; turned
// off, one lane again. Live and frozen, Beats and Seconds, the sum and the difference.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { CompareLane } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import {
  extremePositions,
  levelTargets,
  zeroCrossings,
} from "../../src/view/snap.ts";
import {
  beforeApp,
  byId,
  chips,
  inTimeBase,
  press,
  choice,
  setCompare,
  settle,
  showOnly,
  startApp,
  type Started,
} from "./harness.ts";

// The screen's lanes in this layout, as the owner chose them: equal halves, the tracks in the
// upper, Compare's in the lower. Stated here, not asked of the lane rule this tests.
const tracksLane = { center: 0.5, halfHeight: 0.5 };
const compareLane = { center: -0.5, halfHeight: 0.5 };

const trace = () => byId("trace").getBoundingClientRect();
const screenHeight = () => byId("screen").clientHeight;
const frame = (): Frame => {
  const latest = window.orizonDebug?.lastFrame;
  expect(latest).toBeDefined();
  return latest as Frame;
};

// The top, in CSS px from the screen's, of `value` (a fraction of full scale) in `lane`.
function topOf(
  lane: { center: number; halfHeight: number },
  value: number,
): number {
  return ((1 - (lane.center + value * lane.halfHeight)) / 2) * screenHeight();
}

// A pointer event at (`x`, `y`), fractions of the trace; snapping unless `alt`.
function pointerAt(x: number, y: number, buttons: number, alt = false) {
  const box = trace();
  return {
    pointerId: 1,
    bubbles: true,
    buttons,
    altKey: alt,
    clientX: box.left + x * box.width,
    clientY: box.top + y * box.height,
  };
}

function dragTo(element: HTMLElement, x: number, y: number, alt = false) {
  element.dispatchEvent(
    new PointerEvent("pointerdown", pointerAt(x, y, 1, alt)),
  );
  element.dispatchEvent(
    new PointerEvent("pointermove", pointerAt(x, y, 1, alt)),
  );
  element.dispatchEvent(new PointerEvent("pointerup", pointerAt(x, y, 0, alt)));
}

async function setPressed(id: string, pressed: boolean): Promise<void> {
  if ((byId(id).getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(byId(id));
  }
}

// Drags time cursor `cursor` a little beside a target (crossing or extreme) of `wanted`, at
// height `y`, and expects it to land on that target: one that `rival` has not, and the nearest of
// `wanted`'s to where it is dropped. Snapping to `rival` instead would land on one of its targets,
// or stay where it was dropped.
function expectSnapsTo(
  cursor: HTMLElement,
  shown: Frame,
  wanted: Float32Array | undefined,
  rival: Float32Array | undefined,
  y: number,
): void {
  const targets = (values: Float32Array | undefined) =>
    values === undefined
      ? []
      : [...zeroCrossings(shown, values), ...extremePositions(shown, values)];
  const ours = targets(wanted);
  const theirs = targets(rival);
  const pixel = 1 / trace().width;
  const offset = 2 * pixel;
  const target = ours.find(
    (position) =>
      position > 0.1 &&
      position < 0.9 &&
      theirs.every((other) => Math.abs(other - position) > pixel) &&
      ours.every(
        (other) =>
          other === position ||
          Math.abs(other - (position + offset)) > offset + pixel / 2,
      ),
  );
  expect(target, "a target of the wanted trace alone").toBeDefined();
  dragTo(cursor, (target ?? 0) + offset, y);
  expect(Number.parseFloat(cursor.style.left) / 100).toBeCloseTo(
    target ?? Number.NaN,
    6,
  );
}

const laneLabels = () => [
  ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label"),
];
const levelLabels = () =>
  [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")].filter(
    (label) => label.style.top !== "",
  );
const levelCursors = () => [
  ...byId("level-cursor-lines").querySelectorAll<HTMLElement>(".level-cursor"),
];
const placedTop = (element: HTMLElement) =>
  Number.parseFloat(element.style.top);
const minusInfinity = "−∞";

// Compare's switch, live or frozen, and the time base: each pair of them once, in four sessions
// rather than all eight; Seconds is Trig, where the trigger marker shows.
const cases = [
  { shows: "Sum", state: "live", timeBase: "Beats" },
  { shows: "Sum", state: "frozen", timeBase: "Trig" },
  { shows: "Diff", state: "live", timeBase: "Trig" },
  { shows: "Diff", state: "frozen", timeBase: "Beats" },
] as const;

describe.each(cases)(
  "overlaid with Compare on ($shows), $state, in $timeBase",
  ({ shows, state, timeBase }) => {
    const name = shows === "Sum" ? "Sum" : "Difference";
    let started: Started;

    beforeApp(async () => {
      // Kick (this track), Bass, Track 3 and Kick 2: four tracks overlaid, so several share the
      // tracks' lane.
      started = await startApp();
      started.transport.send({
        type: "setSetting",
        name: "verticalScale",
        value: 0,
      });
      await inTimeBase(timeBase);
      await press(choice("display-mode", "Multi"));
      await press(choice("layout", "Overlay"));
      await setCompare(shows);
      await expect
        .poll(() => window.orizonDebug?.lastFrame?.comparison?.lane)
        .toBe(shows === "Sum" ? CompareLane.sum : CompareLane.difference);
      if (state === "frozen") {
        await userEvent.click(byId("freeze"));
        await expect.poll(chips).toContain("Frozen");
      }
      await settle();
      expect(frame().lanes).toHaveLength(5);
    });

    test("the level labels mark two lanes, each with its zero line", () => {
      const zeros = levelLabels().filter(
        (label) => label.textContent === minusInfinity,
      );
      expect(zeros).toHaveLength(2);
    });

    test("the tracks' legend tops the upper half and Compare's label, with its figure, the lower", () => {
      const [legend, compare] = laneLabels();
      expect(laneLabels()).toHaveLength(2);
      expect(legend?.textContent).toContain("Kick");
      expect(legend?.textContent).not.toContain(name);
      expect(placedTop(legend as HTMLElement)).toBeCloseTo(
        topOf(tracksLane, 1) + 3,
        0,
      );
      expect(compare?.textContent.startsWith(name)).toBe(true);
      expect(compare?.querySelector(".lane-readout")).not.toBeNull();
      expect(placedTop(compare as HTMLElement)).toBeCloseTo(
        topOf(compareLane, 1) + 3,
        0,
      );
    });

    test("the hover reads Compare's lane in the lower half and this track in the upper", async () => {
      await setPressed("hover-toggle", true);
      const readout = byId("hover-readout");
      byId("trace").dispatchEvent(
        new PointerEvent("pointermove", pointerAt(0.5, 0.75, 0)),
      );
      expect(readout.hidden).toBe(false);
      expect(readout.textContent).toContain(name);
      byId("trace").dispatchEvent(
        new PointerEvent("pointermove", pointerAt(0.5, 0.25, 0)),
      );
      expect(readout.textContent).toContain("Kick");
      expect(readout.textContent).not.toContain(name);
      await setPressed("hover-toggle", false);
    });

    test("a time cursor dragged in the lower half snaps to Compare's lane, not this track's", async () => {
      await setPressed("time-cursors", true);
      const cursor = byId("cursor-a");
      // Read and dragged in one task: no frame arrives between.
      const shown = frame();
      expectSnapsTo(cursor, shown, shown.lanes.at(-1), shown.lanes[0], 0.75);
      await setPressed("time-cursors", false);
    });

    test("the level cursors come in a pair for each half, and the lower pair snaps to Compare's lane", async () => {
      await setPressed("level-cursors", true);
      await expect.poll(() => levelCursors().length).toBe(4);
      const line = (lane: number, key: string) =>
        levelCursors().find(
          (each) =>
            each.dataset["lane"] === String(lane) &&
            each.dataset["key"] === key,
        ) as HTMLElement;
      // The same level in each half: the lower pair half a screen below the upper.
      for (const key of ["first", "second"]) {
        expect(placedTop(line(1, key)) - placedTop(line(0, key))).toBeCloseTo(
          screenHeight() / 2,
          0,
        );
      }
      // Compare's highest level, which is none of this track's: a drag a few pixels off it lands on
      // it only if the lower pair snaps to Compare's lane.
      const shown = frame();
      const [highest = 0] = levelTargets(
        shown.lanes.at(-1) ?? new Float32Array(),
      );
      const own = levelTargets(shown.lanes[0] ?? new Float32Array());
      const halfLanePixels = (compareLane.halfHeight * screenHeight()) / 2;
      expect(
        own.every((level) => Math.abs(level - highest) * halfLanePixels > 2),
        "Compare's highest is none of this track's levels",
      ).toBe(true);
      const top = topOf(compareLane, highest);
      dragTo(line(1, "first"), 0.5, (top + 4) / screenHeight());
      expect(placedTop(line(1, "first"))).toBeCloseTo(top, 0);
      await setPressed("level-cursors", false);
    });

    // Hidden in Beats, which never triggers.
    test.runIf(timeBase === "Trig")(
      "the trigger marker sits in the tracks' half, on its scale, and a drag sets the level there",
      async () => {
        const marker = byId("trigger-marker");
        expect(marker.hidden).toBe(false);
        // A quarter of the way up the tracks' half: half the full scale in their lane, where the
        // whole screen's scale would read three quarters.
        const y = topOf(tracksLane, 0.5) / screenHeight();
        dragTo(marker, 0.5, y, true);
        const asked = started.sent
          .filter(
            (message) =>
              message.type === "setSetting" &&
              message["name"] === "triggerLevel",
          )
          .at(-1);
        expect(Number(asked?.["value"])).toBeCloseTo(0.5, 2);
        // Placed again from the level the engine now holds, in the tracks' half.
        await expect
          .poll(() => Math.abs(placedTop(marker) - topOf(tracksLane, 0.5)))
          .toBeLessThan(1);
      },
    );

    test("turned off, the screen is one lane again", async () => {
      await setCompare(false);
      await expect.poll(() => frame().comparison).toBeUndefined();
      await settle();
      expect(
        levelLabels().filter((label) => label.textContent === minusInfinity),
      ).toHaveLength(1);
      expect(laneLabels()).toHaveLength(1);
      expect(laneLabels()[0]?.textContent).not.toContain(name);
    });
  },
);

// Overlaid without Compare, every track shares the one lane, and the cursors snap to this track,
// the one the readouts measure, wherever the host lists it: here second, after the bass.
describe("overlaid without Compare, with this track listed second", () => {
  beforeApp(async () => {
    await startApp({ tracksBefore: 1 });
    await inTimeBase("Trig");
    await press(choice("display-mode", "Multi"));
    await press(choice("layout", "Overlay"));
    await showOnly(["Bass"]);
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.lanes.length)
      .toBe(2);
    await settle();
    expect(frame().comparison).toBeUndefined();
  });

  test("a time cursor snaps to this track, not the first lane's", async () => {
    await setPressed("time-cursors", true);
    const shown = frame();
    expectSnapsTo(byId("cursor-a"), shown, shown.lanes[1], shown.lanes[0], 0.5);
    await setPressed("time-cursors", false);
  });
});
