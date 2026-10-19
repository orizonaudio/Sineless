// The resize grip asks the plugin for the window size the user drags it to.
import { beforeAll, describe, expect, test, vi } from "vitest";

import { byId, startApp, type Started } from "./harness.ts";

function drag(
  grip: HTMLElement,
  from: { x: number; y: number },
  to: readonly { x: number; y: number }[],
  button = 0,
): void {
  const at = (point: { x: number; y: number }) => ({
    pointerId: 1,
    bubbles: true,
    button,
    screenX: point.x,
    screenY: point.y,
  });
  grip.dispatchEvent(
    new PointerEvent("pointerdown", { ...at(from), buttons: 1 }),
  );
  for (const point of to) {
    grip.dispatchEvent(
      new PointerEvent("pointermove", { ...at(point), buttons: 1 }),
    );
  }
  grip.dispatchEvent(new PointerEvent("pointerup", at(to.at(-1) ?? from)));
}

function resizes(app: Started) {
  return app.sent.filter((message) => message.type === "resize");
}

describe("resize grip", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
  });

  test("asks for the window's size plus how far the grip was dragged", async () => {
    const before = resizes(app).length;
    drag(byId("resize-grip"), { x: 500, y: 400 }, [
      { x: 560, y: 430 },
      { x: 800, y: 600 },
    ]);
    await vi.waitFor(() => {
      expect(resizes(app).length).toBeGreaterThan(before);
    });
    expect(resizes(app).at(-1)).toMatchObject({
      width: window.innerWidth + 300,
      height: window.innerHeight + 200,
    });
  });

  test("asks the host once a frame, however often the pointer moves", async () => {
    const grip = byId("resize-grip");
    const before = resizes(app).length;
    const at = (x: number) => ({
      pointerId: 1,
      bubbles: true,
      button: 0,
      buttons: 1,
      screenX: x,
      screenY: 400,
    });
    grip.dispatchEvent(new PointerEvent("pointerdown", at(500)));
    for (const x of [510, 520, 530, 540]) {
      grip.dispatchEvent(new PointerEvent("pointermove", at(x)));
    }
    await new Promise((resolve) => requestAnimationFrame(resolve));
    await new Promise((resolve) => requestAnimationFrame(resolve));
    expect(resizes(app).length).toBe(before + 1);
    expect(resizes(app).at(-1)).toMatchObject({
      width: window.innerWidth + 40,
    });
    grip.dispatchEvent(new PointerEvent("pointerup", at(540)));
  });

  test("asks for a smaller size when dragged in", async () => {
    drag(byId("resize-grip"), { x: 500, y: 400 }, [{ x: 450, y: 380 }]);
    await vi.waitFor(() => {
      expect(resizes(app).at(-1)).toMatchObject({
        width: window.innerWidth - 50,
        height: window.innerHeight - 20,
      });
    });
  });

  test("ignores buttons other than the main one", async () => {
    const before = resizes(app).length;
    drag(byId("resize-grip"), { x: 500, y: 400 }, [{ x: 700, y: 600 }], 2);
    await new Promise((resolve) => requestAnimationFrame(resolve));
    expect(resizes(app).length).toBe(before);
  });

  test("sits in the window's bottom-right corner, over no control", () => {
    const grip = byId("resize-grip").getBoundingClientRect();
    expect(grip.right).toBe(window.innerWidth);
    expect(grip.bottom).toBe(window.innerHeight);
    for (const control of document.querySelectorAll("button, input")) {
      const box = control.getBoundingClientRect();
      const overlaps =
        box.width > 0 &&
        box.left < grip.right &&
        box.right > grip.left &&
        box.top < grip.bottom &&
        box.bottom > grip.top;
      expect(overlaps, control.textContent || control.id).toBe(false);
    }
  });
});
