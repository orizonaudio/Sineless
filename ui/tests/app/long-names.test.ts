// A track's name too long for its place is cut in the middle wherever a lane is named (#425): its
// start and its end stay readable, a screen reader says it whole, and it rolls to its end and
// back under the pointer; for a user who asked for reduced motion it does not move, and the whole
// name shows in a tip. A name that fits is left as it is.
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, settle, showDisplay, startApp } from "./harness.ts";

const long = "Drum Bus Parallel Crush Right";
const alike = "Drum Bus Parallel Crush Left";

function labelNames(): HTMLElement[] {
  return [...byId("lane-labels").querySelectorAll<HTMLElement>(".cut-name")];
}

function nameOf(text: string): HTMLElement {
  const name = labelNames().find((node) =>
    (node.dataset["fullName"] ?? node.textContent).startsWith(text),
  );
  expect(name, text).toBeDefined();
  return name as HTMLElement;
}

function shown(name: HTMLElement): string {
  return name.querySelector(".cut-shown")?.textContent ?? "";
}

async function startWithLongNames(): Promise<void> {
  await startApp({ names: { 2: long, 4: alike } });
  await showDisplay("Multi");
  await settle();
  await expect.poll(() => labelNames().length).toBeGreaterThan(0);
  await expect.poll(() => nameOf(long).classList.contains("cut")).toBe(true);
}

describe("a long track name on a lane label", () => {
  test("is cut in the middle, keeping its start and its end, within its place", async () => {
    await startWithLongNames();
    const name = nameOf(long);
    const [head, tail] = shown(name).split("…");
    expect(head?.length).toBeGreaterThan(3);
    expect(tail?.length).toBeGreaterThan(3);
    expect(long.startsWith(head ?? "-")).toBe(true);
    expect(long.endsWith(tail ?? "-")).toBe(true);
    // In the real font, nothing of the cut name is clipped.
    expect(name.scrollWidth).toBeLessThanOrEqual(name.clientWidth);
    // Two tracks that differ at the end still read apart.
    expect(shown(nameOf(alike))).not.toBe(shown(name));
    expect(shown(nameOf(alike)).endsWith("Left")).toBe(true);
  });

  test("is whole to a screen reader", async () => {
    await startWithLongNames();
    const name = nameOf(long);
    expect(name.querySelector(".cut-shown")?.getAttribute("aria-hidden")).toBe(
      "true",
    );
    expect(name.querySelector(".visually-hidden")?.textContent).toBe(long);
  });

  test("leaves a name that fits as it is", async () => {
    await startWithLongNames();
    const name = nameOf("Kick");
    expect(name.textContent).toBe("Kick");
    expect(name.classList.contains("cut")).toBe(false);
    expect(name.querySelector("[aria-hidden]")).toBeNull();
  });

  test("rolls to its end under the pointer, and is cut again when the pointer leaves", async () => {
    await startWithLongNames();
    const name = nameOf(long);
    const atRest = shown(name);
    const width = name.getBoundingClientRect().width;
    await userEvent.hover(name);
    await expect.poll(() => name.classList.contains("rolling")).toBe(true);
    // The whole name moves inside a box that keeps its width, so nothing beside it moves.
    expect(shown(name)).toBe(long);
    expect(name.getBoundingClientRect().width).toBeCloseTo(width, 0);
    const moving = name.querySelector(".cut-shown")?.getAnimations() ?? [];
    expect(moving).toHaveLength(1);
    // One pass that ends by itself, within five seconds.
    const timing = moving[0]?.effect?.getComputedTiming();
    expect(timing?.iterations).toBe(1);
    expect(Number(timing?.duration)).toBeLessThanOrEqual(5000);
    await userEvent.unhover(name);
    await expect.poll(() => name.classList.contains("rolling")).toBe(false);
    expect(shown(name)).toBe(atRest);
    expect(name.querySelector(".cut-shown")?.getAnimations()).toHaveLength(0);
  });

  test("returns to its cut by itself when the roll ends", async () => {
    await startWithLongNames();
    const name = nameOf(long);
    const atRest = shown(name);
    await userEvent.hover(name);
    await expect.poll(() => name.classList.contains("rolling")).toBe(true);
    for (const roll of name.querySelector(".cut-shown")?.getAnimations() ??
      []) {
      roll.finish();
    }
    await expect.poll(() => name.classList.contains("rolling")).toBe(false);
    expect(shown(name)).toBe(atRest);
  });

  test("lets the pointer through to the screen beside the name", async () => {
    await startWithLongNames();
    const label = nameOf(long).closest<HTMLElement>(".lane-label");
    expect(label).not.toBeNull();
    expect(getComputedStyle(label as HTMLElement).pointerEvents).toBe("none");
    expect(getComputedStyle(nameOf("Kick")).pointerEvents).toBe("none");
  });

  test("does not move for a user who asked for reduced motion, and shows whole in a tip", async () => {
    const reduce = vi
      .spyOn(window, "matchMedia")
      .mockImplementation((query) => ({
        matches: query.includes("prefers-reduced-motion"),
        media: query,
        onchange: null,
        addEventListener: () => undefined,
        removeEventListener: () => undefined,
        addListener: () => undefined,
        removeListener: () => undefined,
        dispatchEvent: () => false,
      }));
    try {
      await startWithLongNames();
      const name = nameOf(long);
      const atRest = shown(name);
      const box = name.getBoundingClientRect();
      name.dispatchEvent(
        new PointerEvent("pointerenter", { pointerId: 1, bubbles: false }),
      );
      name.dispatchEvent(
        new PointerEvent("pointermove", {
          pointerId: 1,
          bubbles: true,
          clientX: box.left + box.width / 2,
          clientY: box.top + box.height / 2,
        }),
      );
      await vi.waitFor(
        () => {
          expect(byId("help-tip").hidden).toBe(false);
          expect(byId("help-tip").textContent).toBe(long);
        },
        { timeout: 5000 },
      );
      expect(name.classList.contains("rolling")).toBe(false);
      expect(shown(name)).toBe(atRest);
    } finally {
      reduce.mockRestore();
    }
  });
});

describe("a long track name in the hover readout", () => {
  test("is cut in the middle as on the lane label", async () => {
    await startApp({ names: { 1: long } });
    await showDisplay("Multi");
    await settle();
    await userEvent.click(byId("hover-toggle"));
    const screen = byId("screen").getBoundingClientRect();
    await userEvent.hover(byId("screen"), {
      position: { x: screen.width / 2, y: 30 },
    });
    await expect.poll(() => byId("hover-readout").hidden).toBe(false);
    const name = byId("hover-readout").querySelector<HTMLElement>(".cut-name");
    expect(name?.classList.contains("cut")).toBe(true);
    expect(name?.dataset["fullName"]).toBe(long);
    expect(
      (name?.querySelector(".cut-shown")?.textContent ?? "").includes("…"),
    ).toBe(true);
  });
});
