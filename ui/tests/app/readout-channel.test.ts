// With the views overlaid, the hover and the time cursors measure the readout channel, the Mid, in
// Single display mode with Sec, whatever the trigger source; the first trace drawn when the Mid is
// not. The trigger source picks only what the trigger reads.
import { afterEach, beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { zeroCrossings } from "../../src/view/snap.ts";
import { byId, choice, settle, startApp, typeInto } from "./harness.ts";

async function setPressed(
  button: HTMLElement,
  pressed: boolean,
): Promise<void> {
  if ((button.getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(button);
  }
  await expect.element(button).toHaveAttribute("aria-pressed", String(pressed));
}

// Shows the views whose toggles are labelled `shown` ("L", "M") and hides the others.
async function showViews(shown: readonly string[]): Promise<void> {
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  const toggles = [...byId("views").querySelectorAll("button")];
  // Shown ones first, so that one view stays shown throughout.
  for (const button of toggles) {
    if (shown.includes(button.textContent)) {
      await setPressed(button, true);
    }
  }
  for (const button of toggles) {
    if (!shown.includes(button.textContent)) {
      await setPressed(button, false);
    }
  }
  // The toggle is named after the views a frame has drawn, so its name changes a frame after the
  // last toggle: close it once that frame has come, not while the click's locator can go stale.
  const drawn = toggles
    .map((button) => button.textContent)
    .filter((label) => shown.includes(label));
  await expect
    .element(byId("views-details"))
    .toHaveAttribute("aria-label", `Views: ${drawn.join(" ")}`);
  await userEvent.click(byId("views-details"));
}

async function useSource(label: string): Promise<void> {
  if (byId("trigger-popover").hidden) {
    await userEvent.click(byId("trigger-details"));
  }
  await userEvent.click(choice("source", label));
  await expect
    .element(choice("source", label))
    .toHaveAttribute("aria-pressed", "true");
  await userEvent.click(byId("trigger-details"));
}

// The hover's title: the name of the trace it reads.
async function hoverTitle(): Promise<string | null | undefined> {
  await userEvent.hover(byId("screen"), { position: { x: 300, y: 80 } });
  await expect.element(byId("hover-readout")).toBeVisible();
  return byId("hover-readout").querySelector("strong")?.textContent;
}

// Drags time cursor A to `x`, a fraction of the screen's width, and returns where it is left.
function dragCursorA(x: number): number {
  const line = byId("cursor-a");
  const screen = byId("screen").getBoundingClientRect();
  const at = {
    pointerId: 1,
    bubbles: true,
    clientX: screen.left + x * screen.width,
    clientY: screen.top + screen.height / 2,
  };
  line.dispatchEvent(new PointerEvent("pointerdown", { ...at, buttons: 1 }));
  line.dispatchEvent(new PointerEvent("pointermove", { ...at, buttons: 1 }));
  line.dispatchEvent(new PointerEvent("pointerup", at));
  return Number.parseFloat(line.style.left) / 100;
}

describe("overlaid views", () => {
  beforeAll(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("layout", "Overlay"));
    // A short span keeps the crossings pixels apart, and the screen quick to draw.
    await typeInto("span-field", "10 ms");
  });

  // Each case on its own: the pointer off the screen, the overlays off, the views thawed.
  afterEach(async () => {
    await userEvent.unhover(byId("screen"));
    await setPressed(byId("hover-toggle"), false);
    await setPressed(byId("time-cursors"), false);
    await setPressed(byId("freeze"), false);
  });

  test.each([
    { views: ["L", "R", "M", "S"], source: "S", reads: "Mid" },
    { views: ["L", "R"], source: "R", reads: "Left" },
  ])(
    "the hover reads the $reads with $views drawn and the trigger on $source",
    async ({ views, source, reads }) => {
      await setPressed(byId("hover-toggle"), true);
      await showViews(views);
      await useSource(source);
      await settle();
      await expect.poll(hoverTitle).toBe(reads);
    },
  );

  test("time cursors snap to the Mid's zero crossings with the trigger on Side", async () => {
    await showViews(["M", "S"]);
    await useSource("S");
    await settle();
    await setPressed(byId("time-cursors"), true);
    await setPressed(byId("freeze"), true);
    await settle();
    const frame = window.orizonDebug?.lastFrame as Frame;
    const mid = frame.views.get(ChannelView.mid);
    const side = frame.views.get(ChannelView.side);
    expect(mid).toBeDefined();
    expect(side).toBeDefined();
    const { width } = byId("screen").getBoundingClientRect();
    const offset = 1 / width;
    const near = (a: number, b: number) => Math.abs(a - b) < 0.01 / width;
    const sides = zeroCrossings(frame, side as Float32Array);
    // A crossing of the Mid with no crossing of Side within three pixels, away from the edges.
    const target = zeroCrossings(frame, mid as Float32Array).find(
      (crossing) =>
        crossing > 0.1 &&
        crossing < 0.9 &&
        sides.every((other) => Math.abs(other - crossing) > 3 * offset),
    );
    expect(target, "a crossing of the Mid alone").toBeDefined();
    const at = target as number;
    expect(near(dragCursorA(at + offset), at)).toBe(true);
  });
});
