// The cursor readout in the screen's corner: a titled section per kind of cursor, a rule between
// them, one label and value per row; A and B are where the time cursors are, read as the hover
// readout reads a point (from the trigger, from the right edge, or on the host's clock).
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, inTimeBase, settle, startApp, type Started } from "./harness.ts";

async function setPressed(id: string, pressed: boolean): Promise<void> {
  if ((byId(id).getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(byId(id));
  }
  await expect
    .element(byId(id))
    .toHaveAttribute("aria-pressed", String(pressed));
}

// A readout's rows by label: the cursor readout's, or the hover readout's.
function rowsOf(id: string): Map<string, string> {
  return new Map(
    [...byId(id).querySelectorAll(".cursor-row")].map((row) => [
      row.querySelector("span")?.textContent ?? "",
      row.querySelector("b")?.textContent ?? "",
    ]),
  );
}

// The hover readout's time at a time cursor's line.
function hoverTimeAt(cursor: "a" | "b"): string | undefined {
  const trace = byId("trace").getBoundingClientRect();
  const x = Number.parseFloat(byId(`cursor-${cursor}`).style.left) / 100;
  byId("trace").dispatchEvent(
    new PointerEvent("pointermove", {
      bubbles: true,
      buttons: 0,
      clientX: trace.left + x * trace.width,
      clientY: trace.top + 0.3 * trace.height,
    }),
  );
  return rowsOf("hover-readout").get("Time");
}

// One wheel step zoomed in at the screen's centre, as a user does with ⌘.
function zoomIn(): void {
  const trace = byId("trace");
  const box = trace.getBoundingClientRect();
  trace.dispatchEvent(
    new WheelEvent("wheel", {
      deltaY: -400,
      metaKey: true,
      clientX: box.left + box.width / 2,
      clientY: box.top + box.height / 2,
      bubbles: true,
      cancelable: true,
    }),
  );
}

describe("the cursor readout", () => {
  test("titles each kind of cursor, a rule between them only when both are on", async () => {
    await startApp();
    await settle();
    await setPressed("time-cursors", true);
    const readout = byId("cursor-readout");
    const titles = () =>
      [...readout.querySelectorAll(".cursor-title")].map(
        (title) => title.textContent,
      );
    await expect.poll(titles).toEqual(["Time cursors"]);
    expect(readout.querySelectorAll(".cursor-rule")).toHaveLength(0);
    await setPressed("level-cursors", true);
    await expect.poll(titles).toEqual(["Time cursors", "Level cursors"]);
    expect(readout.querySelectorAll(".cursor-rule")).toHaveLength(1);
    // The rule sits between the two sections.
    const children = [...readout.children];
    const rule = children.findIndex((child) =>
      child.classList.contains("cursor-rule"),
    );
    expect(children[rule + 1]?.textContent).toBe("Level cursors");
    await setPressed("time-cursors", false);
    await expect.poll(titles).toEqual(["Level cursors"]);
    expect(readout.querySelectorAll(".cursor-rule")).toHaveLength(0);
    expect([...rowsOf("cursor-readout").keys()]).toEqual(["1", "2", "Δ"]);
  });

  // Where A and B are must read as the same point does under the pointer, in every time base,
  // live and frozen, and in a zoomed view, where screen and frame positions differ.
  describe.each(["Trig", "Free", "Beats"])("in %s", (timeBase) => {
    let app: Started;

    async function cursorsAfterZoom(): Promise<void> {
      app = await startApp();
      await inTimeBase(timeBase);
      await userEvent.click(byId("wordmark"));
      await setPressed("hover-toggle", true);
      await settle();
      zoomIn();
      await settle();
      await setPressed("time-cursors", false);
      await setPressed("time-cursors", true);
      await settle();
    }

    function expectCursorsReadAsTheHover(): void {
      const rows = rowsOf("cursor-readout");
      expect([...rows.keys()]).toEqual(
        timeBase === "Beats"
          ? ["A", "B", "Δt", "f", "Note", "Δb", "Δs"]
          : ["A", "B", "Δt", "f", "Note", "Δs"],
      );
      const a = rows.get("A");
      const b = rows.get("B");
      expect(a).not.toBe(b);
      expect(hoverTimeAt("a")).toBe(a);
      expect(hoverTimeAt("b")).toBe(b);
    }

    test("A and B read as the hover does at their lines, live", async () => {
      await cursorsAfterZoom();
      // One frame on screen while both are read.
      app.holdFrames(true);
      try {
        expectCursorsReadAsTheHover();
      } finally {
        app.holdFrames(false);
      }
    });

    test("A and B read as the hover does at their lines, frozen", async () => {
      await cursorsAfterZoom();
      await userEvent.click(byId("freeze"));
      await expect.element(byId("freeze")).toHaveTextContent("Frozen");
      await settle();
      expectCursorsReadAsTheHover();
    });
  });
});
