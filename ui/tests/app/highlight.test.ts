// One overlaid trace highlighted (FR-16.4), through the real page: the trace whose lane label the
// pointer rests on, or, with Hover on, the trace the readout reads over the overlaid traces. The
// page decides it and tells the editor which (traceView's highlight, in native mode), so both
// renderers draw it; in WebGL mode the page's own picture changes. Single and Multi, Compare off,
// on the sum and on the difference, live and frozen; never in Stack, on Compare's lane or with one
// trace; ended by the pointer leaving, by a press and by the labels being rebuilt.
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView, CompareLane } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import type { TraceViewMessage } from "../../../protocol/src/messages.ts";
import { mockTracks } from "../../src/bridge/mock-transport.ts";
import type { ControlMessage } from "../../src/bridge/transport.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";
import { readoutView } from "../../src/view/screen-state.ts";
import {
  beforeApp,
  byId,
  chips,
  choice,
  press,
  setCompare,
  setPressed,
  settle,
  showViews,
  startApp,
  type Started,
} from "./harness.ts";

const frame = (): Frame => {
  const latest = window.orizonDebug?.lastFrame;
  expect(latest).toBeDefined();
  return latest as Frame;
};

// What the page last told the editor is highlighted.
function highlightSent(started: Started): TraceViewMessage["highlight"] {
  const sent: readonly ControlMessage[] = started.sent;
  const views = sent.filter(
    (message) => message.type === "traceView",
  ) as unknown as TraceViewMessage[];
  return views.at(-1)?.highlight ?? null;
}

// The lane labels' chips, each naming one trace.
const chipsShown = () => [
  ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label > span"),
];
const chipNamed = (name: string): HTMLElement => {
  const found = chipsShown().find((chip) => chip.textContent.startsWith(name));
  expect(found, `a label for ${name}`).toBeDefined();
  return found as HTMLElement;
};

// The pointer moved onto `element`, at its centre, with `buttons` held (none unless given).
function rest(element: Element, buttons = 0): void {
  const box = element.getBoundingClientRect();
  element.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons,
      clientX: box.left + box.width / 2,
      clientY: box.top + box.height / 2,
    }),
  );
}

// The pointer moved over the traces, at (`x`, `y`), fractions of the screen from the top left.
function restOnTraces(x: number, y: number): void {
  const canvas = byId("trace");
  const box = canvas.getBoundingClientRect();
  canvas.dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      buttons: 0,
      clientX: box.left + x * box.width,
      clientY: box.top + y * box.height,
    }),
  );
}

const viewNames = (Object.keys(ChannelView) as (keyof typeof ChannelView)[])
  .filter((name) => Number.isNaN(Number(name)))
  .reduce<Record<number, string>>(
    (names, name) => ({ ...names, [ChannelView[name]]: name }),
    {},
  );
const trackId = (name: string) =>
  mockTracks.find((track) => track.name === name)?.id;

// Display mode and Compare, each with one of live and frozen.
const cases = [
  { display: "Single", compare: "off", state: "live" },
  { display: "Multi", compare: "off", state: "frozen" },
  { display: "Multi", compare: "Sum", state: "live" },
  { display: "Multi", compare: "Diff", state: "frozen" },
] as const;

describe.each(cases)(
  "$display, Compare $compare, $state",
  ({ display, compare, state }) => {
    let started: Started;
    // A label of a trace that is not the readout's, and what its highlight names.
    const other =
      display === "Single"
        ? { label: "Right", highlight: { view: "right" } }
        : { label: "Bass", highlight: { lane: trackId("Bass") ?? -1 } };
    const readoutHighlight = () =>
      display === "Single"
        ? { view: viewNames[readoutView(frame())] ?? "" }
        : { lane: trackId("Kick") ?? -1 };

    beforeApp(async () => {
      started = await startApp({ nativeTraces: true });
      await press(choice("display-mode", display));
      await press(choice("layout", "Overlay"));
      if (display === "Single") {
        await showViews(["L", "R"]);
      }
      if (compare !== "off") {
        await setCompare(compare);
        await expect
          .poll(() => window.orizonDebug?.lastFrame?.comparison?.lane)
          .toBe(compare === "Sum" ? CompareLane.sum : CompareLane.difference);
      }
      if (state === "frozen") {
        await userEvent.click(byId("freeze"));
        await expect.poll(chips).toContain("Frozen");
      }
      await settle();
    });

    test("resting on a lane label highlights its trace, and leaving it ends the highlight", async () => {
      await setPressed(byId("hover-toggle"), false);
      rest(chipNamed(other.label));
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
      restOnTraces(0.5, 0.25);
      await expect.poll(() => highlightSent(started)).toBeNull();
      rest(chipNamed(other.label));
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
      byId("screen").dispatchEvent(
        new PointerEvent("pointerleave", { pointerId: 1 }),
      );
      await expect.poll(() => highlightSent(started)).toBeNull();
    });

    test("a press on a label ends the highlight, so a drag from it zooms with nothing highlighted", async () => {
      rest(chipNamed(other.label));
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
      chipNamed(other.label).dispatchEvent(
        new PointerEvent("pointerdown", {
          pointerId: 1,
          bubbles: true,
          button: 0,
          buttons: 1,
        }),
      );
      await expect.poll(() => highlightSent(started)).toBeNull();
      document.dispatchEvent(
        new PointerEvent("pointerup", { pointerId: 1, bubbles: true }),
      );
    });

    // A right press is not a zoom drag, so the screen does not capture the pointer: moves with
    // the button held still reach the chip, and highlight nothing.
    test("a label under the pointer while a button is held highlights nothing", async () => {
      const chip = chipNamed(other.label);
      chip.dispatchEvent(
        new PointerEvent("pointerdown", {
          pointerId: 1,
          bubbles: true,
          button: 2,
          buttons: 2,
        }),
      );
      rest(chip, 2);
      await new Promise((resolve) => requestAnimationFrame(resolve));
      await new Promise((resolve) => requestAnimationFrame(resolve));
      expect(highlightSent(started)).toBeNull();
      document.dispatchEvent(
        new PointerEvent("pointerup", { pointerId: 1, bubbles: true }),
      );
      rest(chip);
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
    });

    test("with Hover on, the readout over the overlaid traces highlights the trace it reads", async () => {
      if (display === "Single") {
        // A readout reading the second trace, so a highlight of the first one fails.
        await press(choice("readout-source", "R"));
        await expect.poll(() => readoutView(frame())).toBe(ChannelView.right);
      }
      await setPressed(byId("hover-toggle"), true);
      restOnTraces(0.5, compare === "off" ? 0.5 : 0.25);
      await expect.poll(() => byId("hover-readout").hidden).toBe(false);
      await expect
        .poll(() => highlightSent(started))
        .toEqual(readoutHighlight());
      await setPressed(byId("hover-toggle"), false);
      await expect.poll(() => highlightSent(started)).toBeNull();
    });

    test("with Hover on, resting on a label hides the readout and highlights the label's trace", async () => {
      await setPressed(byId("hover-toggle"), true);
      restOnTraces(0.5, compare === "off" ? 0.5 : 0.25);
      await expect.poll(() => byId("hover-readout").hidden).toBe(false);
      rest(chipNamed(other.label));
      await expect.poll(() => byId("hover-readout").hidden).toBe(true);
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
      await setPressed(byId("hover-toggle"), false);
    });

    if (compare !== "off") {
      test("Compare's lane and its label highlight nothing", async () => {
        await setPressed(byId("hover-toggle"), true);
        restOnTraces(0.5, 0.75);
        await expect.poll(() => byId("hover-readout").hidden).toBe(false);
        await settle();
        expect(highlightSent(started)).toBeNull();
        rest(chipNamed(compare === "Sum" ? "Sum" : "Difference"));
        await settle();
        expect(highlightSent(started)).toBeNull();
        await setPressed(byId("hover-toggle"), false);
      });
    }

    test("the highlight stays while frames come with the pointer at rest", async () => {
      rest(chipNamed(other.label));
      await expect.poll(() => highlightSent(started)).toEqual(other.highlight);
      await settle(10);
      expect(highlightSent(started)).toEqual(other.highlight);
      restOnTraces(0.5, 0.25);
    });

    test("stacked, nothing is highlighted", async () => {
      await press(choice("layout", "Stack"));
      await settle();
      rest(chipNamed(other.label));
      await setPressed(byId("hover-toggle"), true);
      restOnTraces(0.5, 0.1);
      await settle();
      expect(highlightSent(started)).toBeNull();
      await setPressed(byId("hover-toggle"), false);
      await press(choice("layout", "Overlay"));
      await settle();
    });
  },
);

describe("with one trace drawn", () => {
  let started: Started;
  beforeApp(async () => {
    started = await startApp({ nativeTraces: true });
    await press(choice("display-mode", "Single"));
    await press(choice("layout", "Overlay"));
    await showViews(["L"]);
    await settle();
  });

  test("resting on its label or reading it highlights nothing", async () => {
    rest(chipNamed("Left"));
    await setPressed(byId("hover-toggle"), true);
    restOnTraces(0.5, 0.5);
    await settle();
    expect(highlightSent(started)).toBeNull();
  });
});

describe("the labels rebuilt under the pointer", () => {
  let started: Started;
  beforeApp(async () => {
    started = await startApp({ nativeTraces: true });
    await press(choice("display-mode", "Multi"));
    await press(choice("layout", "Overlay"));
    await settle();
  });

  test("end the highlight until the pointer moves again", async () => {
    rest(chipNamed("Bass"));
    await expect
      .poll(() => highlightSent(started))
      .toEqual({ lane: trackId("Bass") });
    // Compare on rebuilds the labels, each with its match readout.
    await setCompare("Sum");
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.comparison)
      .toBeDefined();
    await expect.poll(() => highlightSent(started)).toBeNull();
    rest(chipNamed("Bass"));
    await expect
      .poll(() => highlightSent(started))
      .toEqual({ lane: trackId("Bass") });
  });
});

// In WebGL mode the page draws the highlight itself, frozen as live: the picture each draw leaves,
// read straight after it (the canvas is cleared once composited).
describe("the page's own picture", () => {
  let latest = "";
  let draws = 0;
  beforeApp(async () => {
    const draw = Object.getOwnPropertyDescriptor(
      TraceRenderer.prototype,
      "draw",
    )?.value as (this: TraceRenderer, frame: Frame) => void;
    vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
      this: TraceRenderer,
      shown: Frame,
    ) {
      draw.call(this, shown);
      const canvas = byId("trace") as HTMLCanvasElement;
      const gl = canvas.getContext("webgl2");
      if (gl === null) {
        return;
      }
      const pixels = new Uint8Array(canvas.width * canvas.height * 4);
      gl.readPixels(
        0,
        0,
        canvas.width,
        canvas.height,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        pixels,
      );
      let sum = 0;
      pixels.forEach((value, index) => {
        sum = (sum + value * ((index % 251) + 1)) % 1_000_000_007;
      });
      latest = String(sum);
      draws++;
    });
    await startApp();
    await press(choice("display-mode", "Multi"));
    await press(choice("layout", "Overlay"));
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).toContain("Frozen");
    await settle();
  });

  test("a frozen screen redraws when a highlight starts and ends", async () => {
    await settle();
    const before = latest;
    const drawn = draws;
    rest(chipNamed("Bass"));
    await expect.poll(() => draws).toBeGreaterThan(drawn);
    await expect.poll(() => latest).not.toBe(before);
    restOnTraces(0.5, 0.5);
    await expect.poll(() => latest).toBe(before);
  });
});

// The labels' chips take the pointer, for the highlight; a drag, a double-click, ⌘ + scroll and a
// swipe that start on one still act on the screen, as they do beside it, in either layout.
describe.each(["Overlay", "Stack"] as const)(
  "gestures that start on a lane label, %s",
  (layout) => {
    beforeApp(async () => {
      await startApp();
      await press(choice("display-mode", "Multi"));
      await press(choice("layout", layout));
      await settle();
    });

    const shownWidth = () => {
      const shown = window.orizonDebug?.lastFrame;
      return (shown?.viewEnd ?? 1) - (shown?.viewStart ?? 0);
    };
    const shownStart = () => window.orizonDebug?.lastFrame?.viewStart ?? 0;
    const centre = (element: Element) => {
      const box = element.getBoundingClientRect();
      return { x: box.left + box.width / 2, y: box.top + box.height / 2 };
    };
    const reset = async () => {
      chipNamed("Bass").dispatchEvent(
        new MouseEvent("dblclick", { bubbles: true }),
      );
      await expect.poll(shownWidth).toBeCloseTo(1, 6);
    };

    test("the chip is what the pointer is over", () => {
      const chip = chipNamed("Bass");
      const { x, y } = centre(chip);
      expect(chip.contains(document.elementFromPoint(x, y))).toBe(true);
    });

    test("a drag from it zooms to the selection, and a double-click on it resets", async () => {
      const chip = chipNamed("Bass");
      const { x, y } = centre(chip);
      const box = byId("trace").getBoundingClientRect();
      const at = (clientX: number, buttons: number) => ({
        pointerId: 1,
        bubbles: true,
        button: 0,
        buttons,
        clientX,
        clientY: y,
      });
      const to = box.left + 0.9 * box.width;
      chip.dispatchEvent(new PointerEvent("pointerdown", at(x, 1)));
      chip.dispatchEvent(new PointerEvent("pointermove", at(to, 1)));
      chip.dispatchEvent(new PointerEvent("pointerup", at(to, 0)));
      const expected = 0.9 - (x - box.left) / box.width;
      await expect.poll(shownWidth).toBeCloseTo(expected, 2);
      await reset();
    });

    test("⌘ + scroll on it zooms, and a swipe on it pans", async () => {
      const chip = chipNamed("Bass");
      const { x, y } = centre(chip);
      chip.dispatchEvent(
        new WheelEvent("wheel", {
          deltaY: -100,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          metaKey: true,
          clientX: x,
          clientY: y,
          bubbles: true,
          cancelable: true,
        }),
      );
      await expect.poll(shownWidth).toBeLessThan(0.99);
      const start = shownStart();
      chip.dispatchEvent(
        new WheelEvent("wheel", {
          deltaX: 0.1 * byId("screen").clientWidth,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          clientX: x,
          clientY: y,
          bubbles: true,
          cancelable: true,
        }),
      );
      await expect.poll(shownStart).not.toBeCloseTo(start, 6);
      await reset();
    });
  },
);
