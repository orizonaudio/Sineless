// A drag the browser cancels (a touch it takes for a pan, or the pointer captured elsewhere) ends
// there: moving the pointer or clicking later changes nothing it was dragging.
import { beforeAll, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, startApp, type Started } from "./harness.ts";

const endings = ["pointercancel", "lostpointercapture"] as const;

// Sends a pointer event to `target` at (`x`, `y`), fractions of the screen.
function pointer(
  type: string,
  target: Element,
  x: number,
  y = 0.5,
  pointerId = 1,
): void {
  const screen = byId("screen").getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent(type, {
      pointerId,
      bubbles: true,
      button: 0,
      buttons: type === "pointerup" ? 0 : 1,
      clientX: screen.left + x * screen.width,
      clientY: screen.top + y * screen.height,
    }),
  );
}

async function press(id: string): Promise<void> {
  if (byId(id).getAttribute("aria-pressed") !== "true") {
    await userEvent.click(byId(id));
  }
}

describe("a cancelled drag", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
    await press("time-cursors");
    await press("level-cursors");
    await settle();
  });

  test.each(endings)(
    "zooms to no selection after %s, and leaves the next click a click",
    (ending) => {
      const trace = byId("trace");
      const selection = byId("selection");
      const before = app.sent.length;
      pointer("pointerdown", trace, 0.2);
      pointer("pointermove", trace, 0.6);
      expect(selection.hidden).toBe(false);
      pointer(ending, trace, 0.6);
      expect(selection.hidden).toBe(true);
      pointer("pointermove", trace, 0.8);
      expect(selection.hidden).toBe(true);
      pointer("pointerdown", trace, 0.5);
      pointer("pointerup", trace, 0.5);
      expect(
        app.sent.slice(before).map((message) => message.type),
      ).not.toContain("view");
    },
  );

  test.each(endings)("leaves a time cursor where it was after %s", (ending) => {
    const line = byId("cursor-a");
    pointer("pointerdown", line, 0.3);
    pointer("pointermove", line, 0.3);
    pointer(ending, line, 0.3);
    const left = line.style.left;
    pointer("pointermove", line, 0.7);
    expect(line.style.left).toBe(left);
  });

  test.each(endings)(
    "leaves a level cursor where it was after %s",
    (ending) => {
      const line = byId("level-cursor-lines").querySelector(".level-cursor");
      expect(line).not.toBeNull();
      const cursor = line as HTMLElement;
      pointer("pointerdown", cursor, 0.5, 0.3);
      pointer("pointermove", cursor, 0.5, 0.3);
      pointer(ending, cursor, 0.5, 0.3);
      const top = cursor.style.top;
      pointer("pointermove", cursor, 0.5, 0.7);
      expect(cursor.style.top).toBe(top);
    },
  );

  test.each(endings)(
    "leaves the trigger level where it was after %s",
    (ending) => {
      const marker = byId("trigger-marker");
      pointer("pointerdown", marker, 0.5, 0.4);
      pointer("pointermove", marker, 0.5, 0.4);
      pointer(ending, marker, 0.5, 0.4);
      const before = app.sent.length;
      pointer("pointermove", marker, 0.5, 0.2);
      expect(
        app.sent
          .slice(before)
          .filter((message) => message["name"] === "triggerLevel"),
      ).toEqual([]);
    },
  );

  // A touch or pen drag the browser may take for a pan or zoom is cancelled: on what the user
  // drags, the page takes every touch itself.
  test("leaves no touch drag on the screen, its cursors, the trigger marker or a value field to the browser", () => {
    const dragged = [
      byId("screen"),
      byId("cursor-a"),
      byId("cursor-b"),
      byId("trigger-marker"),
      ...byId("level-cursor-lines").querySelectorAll<HTMLElement>(
        ".level-cursor",
      ),
      ...document.querySelectorAll<HTMLElement>(".field"),
    ];
    expect(document.querySelectorAll(".field").length).toBeGreaterThan(0);
    for (const element of dragged) {
      expect(
        getComputedStyle(element).touchAction,
        element.id || element.className,
      ).toBe("none");
    }
  });

  // A second finger on the screen while one drags a selection (a pinch on a touch screen).
  test("zooms to the first finger's selection only, whatever a second finger does", () => {
    const screen = byId("screen");
    const trace = byId("trace");
    // Synthetic pointers other than the mouse have nothing to capture.
    const capture = vi
      .spyOn(screen, "setPointerCapture")
      .mockImplementation(() => undefined);
    trace.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    const before = app.sent.length;
    pointer("pointerdown", trace, 0.2, 0.5, 1);
    pointer("pointerdown", trace, 0.5, 0.5, 2);
    pointer("pointermove", trace, 0.9, 0.5, 2);
    pointer("pointerup", trace, 0.9, 0.5, 2);
    pointer("pointermove", trace, 0.6, 0.5, 1);
    pointer("pointerup", trace, 0.6, 0.5, 1);
    capture.mockRestore();
    const views = app.sent
      .slice(before)
      .filter((message) => message.type === "view");
    expect(views).toHaveLength(1);
    expect(views[0]?.["start"]).toBeCloseTo(0.2);
    expect(views[0]?.["end"]).toBeCloseTo(0.6);
  });
});
