// A drag the browser cancels (a touch it takes for a pan, or the pointer captured elsewhere) ends
// there: moving the pointer or clicking later changes nothing it was dragging.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, startApp, type Started } from "./harness.ts";

const endings = ["pointercancel", "lostpointercapture"] as const;

// Sends a pointer event to `target` at (`x`, `y`), fractions of the screen.
function pointer(type: string, target: Element, x: number, y = 0.5): void {
  const screen = byId("screen").getBoundingClientRect();
  target.dispatchEvent(
    new PointerEvent(type, {
      pointerId: 1,
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
});
