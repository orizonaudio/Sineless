// With the views overlaid, the hover and the time cursors measure the readout channel, which the
// readouts' popover in the status bar chooses among the views drawn; the first view drawn when the
// one chosen is not. The trigger source picks only what the trigger reads.
import { afterEach, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { zeroCrossings } from "../../src/view/snap.ts";
import {
  beforeApp,
  byId,
  choice,
  press,
  setPressed,
  settle,
  showViews,
  startApp,
  typeInto,
  useReadout,
} from "./harness.ts";

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
  beforeApp(async () => {
    await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Trig"));
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
    { views: ["L", "R", "M", "S"], readout: "S", reads: "Side" },
    { views: ["L", "R", "M", "S"], readout: "R", reads: "Right" },
  ])(
    "the hover reads the $reads with $views drawn and the readouts on $readout",
    async ({ views, readout, reads }) => {
      await setPressed(byId("hover-toggle"), true);
      await showViews(views);
      await press(choice("source", "M"));
      await useReadout(readout);
      await settle();
      await expect.poll(hoverTitle).toBe(reads);
    },
  );

  // Left and the Mid are drawn too, before the Side: snapping to the first drawn trace, or to the
  // trigger's Mid, would pick another, so only snapping to the readout channel finds the Side's
  // crossing.
  test("time cursors snap to the readout channel's zero crossings", async () => {
    await showViews(["L", "M", "S"]);
    await press(choice("source", "M"));
    await useReadout("S");
    await settle();
    await setPressed(byId("time-cursors"), true);
    await setPressed(byId("freeze"), true);
    await settle();
    const frame = window.orizonDebug?.lastFrame as Frame;
    const side = frame.views.get(ChannelView.side);
    const others = [ChannelView.left, ChannelView.mid].map((view) =>
      frame.views.get(view),
    );
    expect(side).toBeDefined();
    expect(others).not.toContain(undefined);
    const { width } = byId("screen").getBoundingClientRect();
    const offset = 1 / width;
    const near = (a: number, b: number) => Math.abs(a - b) < 0.01 / width;
    const crossings = others.flatMap((samples) =>
      zeroCrossings(frame, samples as Float32Array),
    );
    // A crossing of the Side with no crossing of Left or the Mid within three pixels, away from
    // the edges.
    const target = zeroCrossings(frame, side as Float32Array).find(
      (crossing) =>
        crossing > 0.1 &&
        crossing < 0.9 &&
        crossings.every((other) => Math.abs(other - crossing) > 3 * offset),
    );
    expect(target, "a crossing of the Side alone").toBeDefined();
    const at = target as number;
    expect(near(dragCursorA(at + offset), at)).toBe(true);
  });
});
