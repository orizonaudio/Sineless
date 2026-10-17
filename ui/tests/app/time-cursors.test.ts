// Time cursors dragged over stacked lanes snap to the trace under the pointer, in every display
// mode: a crossing or peak of another lane is not on the trace being measured.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { DisplayMode, ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { lanes } from "../../src/render/trace-geometry.ts";
import { extremePositions, zeroCrossings } from "../../src/view/snap.ts";
import { byId, choice, chips, settle, startApp, typeInto } from "./harness.ts";

// The display modes, as the display-mode control labels them.
const modes = Object.keys(DisplayMode).map(
  (name) => name.charAt(0).toUpperCase() + name.slice(1),
);

// The traces in lane order: the tracks' lanes, or Single's views.
const traces = (frame: Frame): Float32Array[] =>
  frame.lanes.length > 0 ? [...frame.lanes] : [...frame.views.values()];

// Where a time cursor can snap to on `values`: its zero crossings and its peaks.
const targets = (frame: Frame, values: Float32Array): number[] => [
  ...zeroCrossings(frame, values),
  ...extremePositions(frame, values),
];

async function setPressed(id: string, pressed: boolean): Promise<void> {
  if ((byId(id).getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(byId(id));
  }
  await expect
    .element(byId(id))
    .toHaveAttribute("aria-pressed", String(pressed));
}

// Drags time cursor A to (`x`, `y`), fractions of the screen, and returns where it is left, as
// a fraction of the screen's width.
function dragCursorA(x: number, y: number): number {
  const line = byId("cursor-a");
  const screen = byId("screen").getBoundingClientRect();
  const at = {
    pointerId: 1,
    bubbles: true,
    clientX: screen.left + x * screen.width,
    clientY: screen.top + y * screen.height,
  };
  line.dispatchEvent(new PointerEvent("pointerdown", { ...at, buttons: 1 }));
  line.dispatchEvent(new PointerEvent("pointermove", { ...at, buttons: 1 }));
  line.dispatchEvent(new PointerEvent("pointerup", at));
  return Number.parseFloat(line.style.left) / 100;
}

describe("time cursors over stacked lanes", () => {
  beforeAll(async () => {
    await startApp();
  });

  test.each(modes)(
    "snap to the trace in the lane under the pointer in %s",
    async (mode) => {
      await setPressed("freeze", false);
      await userEvent.click(choice("display-mode", mode));
      await userEvent.click(choice("layout", "Stack"));
      // Single shows L and R by default: two lanes, of different traces. A short span keeps
      // each trace's crossings pixels apart.
      await userEvent.click(choice("time-base", "Sec"));
      await typeInto("span-field", "10 ms");
      await setPressed("time-cursors", true);
      await settle();
      await setPressed("freeze", true);
      await expect.poll(chips).toContain("Frozen");
      await settle();
      const frame = window.orizonDebug?.lastFrame;
      expect(frame).toBeDefined();
      const shown = traces(frame as Frame);
      expect(shown.length).toBeGreaterThan(1);
      const laneList = lanes(shown.length, ViewLayout.stacked);
      const { width, height } = byId("screen").getBoundingClientRect();
      // Two pixels inside a lane's edge.
      const inset = 2 / height;
      // A pixel beside a crossing or peak: well within snapping reach.
      const offset = 1 / width;
      // The cursor's place is read back from its style, to a hundredth of a pixel.
      const near = (a: number, b: number) => Math.abs(a - b) < 0.01 / width;
      const all = shown.map((values) => targets(frame as Frame, values));
      const missed: string[] = [];
      let tested = 0;
      all.forEach((own, index) => {
        // A target of this lane alone, with no other of its own within two pixels: the pointer a
        // pixel beside it, towards the middle, snaps to it on this lane, and away from it (or
        // nowhere) on any other.
        const target = own.find(
          (candidate) =>
            own.every(
              (other) =>
                other === candidate || Math.abs(other - candidate) > 2 * offset,
            ) &&
            all.every(
              (others, lane) =>
                lane === index ||
                !others.some((other) => near(other, candidate)),
            ),
        );
        const lane = laneList[index];
        // A lane whose every crossing and peak another lane shares, as Compare's sum is while the
        // tracks checked are silent and it is this track's trace, cannot tell the lanes apart:
        // snapping to either is right.
        if (target === undefined || lane === undefined) {
          return;
        }
        tested += 1;
        // Just inside the lane's top and bottom edges, where a pointer is closest to another lane.
        const top = (1 - (lane.center + lane.halfHeight)) / 2;
        const bottom = (1 - (lane.center - lane.halfHeight)) / 2;
        for (const y of [top + inset, bottom - inset]) {
          const left = dragCursorA(
            target < 0.5 ? target + offset : target - offset,
            y,
          );
          if (!near(left, target)) {
            missed.push(
              `lane ${String(index)} at y ${y.toFixed(3)}: at ${String(left)}, not ${String(target)}`,
            );
          }
        }
      });
      expect(missed).toEqual([]);
      // Two lanes at least, so that a cursor snapping to one lane whatever the pointer fails.
      expect(tested, "lanes with a target of their own").toBeGreaterThanOrEqual(
        2,
      );
    },
  );
});
