// A dragged level cursor, trigger marker, time cursor or zoom selection, and the hover line, sit
// under the pointer, near either edge of
// the screen as at its middle, and on a screen a fractional number of pixels tall, as Windows gives
// at 125% and 150%: the screen's border is not part of where the pointer is.
import { afterAll, beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, choice, settle, startApp } from "./harness.ts";

// How far a dragged line may sit from the pointer (CSS px).
const tolerance = 0.25;

const trace = () => byId("trace").getBoundingClientRect();

// Drags `element` to (`x`, `y`), fractions of the trace (the screen inside its border), with
// Alt held so nothing snaps.
function dragTo(element: HTMLElement, x: number, y: number): void {
  const box = trace();
  const at = {
    pointerId: 1,
    bubbles: true,
    altKey: true,
    clientX: box.left + x * box.width,
    clientY: box.top + y * box.height,
  };
  element.dispatchEvent(new PointerEvent("pointerdown", { ...at, buttons: 1 }));
  element.dispatchEvent(new PointerEvent("pointermove", { ...at, buttons: 1 }));
  element.dispatchEvent(new PointerEvent("pointerup", at));
}

async function setPressed(id: string, pressed: boolean): Promise<void> {
  if ((byId(id).getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(byId(id));
  }
}

// The level cursor line `key` of lane `lane`.
function levelLine(lane: number, key = "first"): HTMLElement {
  const line = byId("level-cursor-lines").querySelector<HTMLElement>(
    `.level-cursor[data-lane="${String(lane)}"][data-key="${key}"]`,
  );
  expect(line, `level cursor ${key} of lane ${String(lane)}`).not.toBeNull();
  return line as HTMLElement;
}

// The pointer's height into the trace, and where `element` was placed, in px from the trace's top.
function pointerTop(y: number): number {
  return y * trace().height;
}
const placedTop = (element: HTMLElement) =>
  Number.parseFloat(element.style.top);

// Where `element` was placed, in px from the trace's left, whether it is placed in % or px.
function placedLeft(element: HTMLElement): number {
  const left = element.style.left;
  return left.endsWith("%")
    ? (Number.parseFloat(left) / 100) * trace().width
    : Number.parseFloat(left);
}

// Rests the pointer, no button held, at (`x`, `y`), fractions of the trace.
function hoverAt(x: number, y: number): void {
  const box = trace();
  byId("trace").dispatchEvent(
    new PointerEvent("pointermove", {
      bubbles: true,
      buttons: 0,
      clientX: box.left + x * box.width,
      clientY: box.top + y * box.height,
    }),
  );
}

// The centre of the trace column (one canvas pixel) at `x`, as a fraction of the trace.
function columnCentre(x: number): number {
  const columns = (byId("trace") as HTMLCanvasElement).width;
  return (Math.floor(x * columns) + 0.5) / columns;
}

function leaveScreen(): void {
  byId("screen").dispatchEvent(new PointerEvent("pointerleave"));
}

async function dragMarker(y: number): Promise<void> {
  const marker = byId("trigger-marker");
  dragTo(marker, 0.5, y);
  await expect
    .poll(() => Math.abs(placedTop(marker) - pointerTop(y)))
    .toBeLessThan(tolerance);
}

describe("dragging on the screen", () => {
  beforeAll(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    await setPressed("level-cursors", true);
    await setPressed("time-cursors", true);
    await settle();
  });

  afterAll(() => {
    const app = document.querySelector(".app") as HTMLElement;
    app.style.height = "";
    app.style.width = "";
  });

  test.each([0.1, 0.9])(
    "an overlaid level cursor sits under the pointer at %s of the height",
    async (y) => {
      await userEvent.click(choice("layout", "Overlay"));
      await settle();
      const line = levelLine(0);
      dragTo(line, 0.5, y);
      expect(Math.abs(placedTop(line) - pointerTop(y))).toBeLessThan(tolerance);
    },
  );

  // Two lanes: the upper one's top edge, the lower one's bottom edge.
  test.each([
    [0, 0.1],
    [1, 0.9],
  ])(
    "a stacked level cursor in lane %s sits under the pointer at %s of the height",
    async (lane, y) => {
      await userEvent.click(choice("layout", "Stack"));
      await settle();
      const line = levelLine(lane);
      dragTo(line, 0.5, y);
      expect(Math.abs(placedTop(line) - pointerTop(y))).toBeLessThan(tolerance);
    },
  );

  test.each([0.1, 0.9])(
    "the overlaid trigger marker sits under the pointer at %s of the height",
    async (y) => {
      await userEvent.click(choice("layout", "Overlay"));
      await settle();
      await dragMarker(y);
    },
  );

  test("the stacked trigger marker sits under the pointer near its lane's edges", async () => {
    await userEvent.click(choice("layout", "Stack"));
    await settle();
    // Two lanes, each a quarter of the height either side of its centre; the marker's lane is
    // the one it sits in.
    const height = trace().height;
    const center =
      placedTop(byId("trigger-marker")) < height / 2
        ? height / 4
        : (3 * height) / 4;
    for (const offset of [-0.8, 0.8]) {
      await dragMarker((center + (offset * height) / 4) / height);
    }
  });

  test.each([0.1, 0.9])(
    "a time cursor sits under the pointer at %s of the width",
    (x) => {
      const line = byId("cursor-a");
      dragTo(line, x, 0.5);
      const left = (Number.parseFloat(line.style.left) / 100) * trace().width;
      expect(Math.abs(left - x * trace().width)).toBeLessThan(tolerance);
    },
  );

  // The hover line, drawn at the centre of the column read: the pointer rests on one, a canvas
  // pixel wide, so the line belongs under it.
  test.each([0.1, 0.9])(
    "the hover line sits under the pointer at %s of the width",
    async (x) => {
      await setPressed("hover-toggle", true);
      try {
        const at = columnCentre(x);
        hoverAt(at, 0.5);
        const line = byId("hover-line");
        await expect.poll(() => line.hidden).toBe(false);
        expect(Math.abs(placedLeft(line) - at * trace().width)).toBeLessThan(
          tolerance,
        );
      } finally {
        leaveScreen();
        await setPressed("hover-toggle", false);
      }
    },
  );

  // A zoom selection dragged across most of the screen covers what the pointer crossed.
  test("a zoom selection spans from where the pointer went down to where it is", () => {
    const box = trace();
    const at = (x: number) => ({
      pointerId: 1,
      bubbles: true,
      button: 0,
      buttons: 1,
      clientX: box.left + x * box.width,
      clientY: box.top + box.height / 2,
    });
    const canvas = byId("trace");
    const selection = byId("selection");
    canvas.dispatchEvent(new PointerEvent("pointerdown", at(0.1)));
    canvas.dispatchEvent(new PointerEvent("pointermove", at(0.9)));
    try {
      expect(selection.hidden).toBe(false);
      const left = placedLeft(selection);
      const right =
        left + (Number.parseFloat(selection.style.width) / 100) * box.width;
      expect(Math.abs(left - 0.1 * box.width)).toBeLessThan(tolerance);
      expect(Math.abs(right - 0.9 * box.width)).toBeLessThan(tolerance);
    } finally {
      canvas.dispatchEvent(new PointerEvent("pointercancel", at(0.9)));
      byId("screen").dispatchEvent(
        new MouseEvent("dblclick", { bubbles: true }),
      );
    }
  });

  test("on a screen a fractional number of pixels tall, the level cursor and marker sit under the pointer", async () => {
    await userEvent.click(choice("layout", "Overlay"));
    const app = document.querySelector(".app") as HTMLElement;
    app.style.height = `${String(Math.floor(app.getBoundingClientRect().height) - 0.6)}px`;
    await settle();
    expect(trace().height % 1, "a fractional trace height").not.toBe(0);
    const line = levelLine(0);
    dragTo(line, 0.5, 0.9);
    expect(Math.abs(placedTop(line) - pointerTop(0.9))).toBeLessThan(tolerance);
    await dragMarker(0.9);
  });

  test("on a screen a fractional number of pixels wide and tall, the hover line sits under the pointer and spans the lane", async () => {
    await userEvent.click(choice("layout", "Overlay"));
    const app = document.querySelector(".app") as HTMLElement;
    const box = app.getBoundingClientRect();
    app.style.width = `${String(Math.floor(box.width) - 0.6)}px`;
    app.style.height = `${String(Math.floor(box.height) - 0.6)}px`;
    await settle();
    expect(trace().width % 1, "a fractional trace width").not.toBe(0);
    expect(trace().height % 1, "a fractional trace height").not.toBe(0);
    await setPressed("hover-toggle", true);
    try {
      const at = columnCentre(0.9);
      hoverAt(at, 0.5);
      const line = byId("hover-line");
      await expect.poll(() => line.hidden).toBe(false);
      expect(Math.abs(placedLeft(line) - at * trace().width)).toBeLessThan(
        tolerance,
      );
      // The overlaid lane is the whole height.
      const bottom = line.getBoundingClientRect().bottom - trace().top;
      expect(Math.abs(bottom - trace().height)).toBeLessThan(tolerance);
    } finally {
      leaveScreen();
      await setPressed("hover-toggle", false);
    }
  });
});
