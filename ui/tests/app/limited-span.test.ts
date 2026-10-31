// A Seconds span longer than the capture keeps (a host changed the sample rate while processing)
// shows what is kept: the time axis and a frozen pan measure that, not the Span setting.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { timeLabels as axisTimeLabels } from "../../src/view/axis-labels.ts";
import { formatSeconds, parseQuantity } from "../../src/view/format.ts";
import { fullView, type ViewWindow } from "../../src/view/zoom.ts";
import {
  byId,
  choice,
  chips,
  settle,
  startApp,
  swipe,
  typeInto,
} from "./harness.ts";

const kept = 2.4;

function timeLabels(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.firstChild?.textContent ?? "")
    .filter((text) => text.endsWith("s"));
}

// The time labels in seconds, in screen order.
function labelSeconds(): number[] {
  return timeLabels().map((text) => {
    const seconds = parseQuantity(text, "time");
    expect(seconds, text).toBeDefined();
    return seconds ?? Number.NaN;
  });
}

// Whether the labels mark a span of `span` seconds: within the rounding of the part a
// free-running window shows to whole samples and of the labels to three digits.
function marksSpan(view: ViewWindow, span: number): boolean {
  const expected = axisTimeLabels(span, true, view).map((label) =>
    parseQuantity(label.text, "time"),
  );
  const shown = labelSeconds();
  return (
    shown.length === expected.length &&
    shown.every(
      (seconds, index) =>
        Math.abs(seconds - (expected[index] ?? 0)) <= 0.01 * span,
    )
  );
}

describe("a span longer than the capture keeps", () => {
  beforeAll(async () => {
    await startApp({ keptSeconds: kept });
  });

  // Both spans run free: only the span the frame shows tells the axis to change.
  test("labels the time axis with the span shown", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await typeInto("span-field", "2 s");
    await settle();
    await expect.poll(() => marksSpan(fullView, 2)).toBe(true);
    await typeInto("span-field", "4 s");
    await expect.poll(chips).toContain("Span limited");
    await expect.poll(() => marksSpan(fullView, kept)).toBe(true);
  });

  // Zoomed far into the limited free-running span, the frame holds a few samples: the axis still
  // measures the span shown, not that part of it.
  test("keeps the axis to the span shown when zoomed in", async () => {
    const trace = byId("trace");
    const bounds = trace.getBoundingClientRect();
    const width = () => {
      const frame = window.orizonDebug?.lastFrame;
      return frame === undefined ? 1 : frame.viewEnd - frame.viewStart;
    };
    for (let step = 0; step < 40 && width() > 1 / 128; step++) {
      trace.dispatchEvent(
        new WheelEvent("wheel", {
          deltaY: -400,
          ctrlKey: true,
          clientX: bounds.left + bounds.width / 2,
          clientY: bounds.top + bounds.height / 2,
          bubbles: true,
          cancelable: true,
        }),
      );
      await settle(1);
    }
    expect(width()).toBeLessThanOrEqual(1 / 128);
    await settle();
    const frame = window.orizonDebug?.lastFrame;
    const view = { start: frame?.viewStart ?? 0, end: frame?.viewEnd ?? 1 };
    expect(marksSpan(view, kept), timeLabels().join(", ")).toBe(true);
  });

  // Frozen, the held audio reaches further back than the limited span shows (Multi and Compare
  // keep a margin for other tracks): a swipe of a quarter of the screen pans a quarter of what
  // the screen shows.
  test("pans a frozen view by the span shown", async () => {
    byId("trace").dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).toContain("Frozen");
    await settle();
    swipe(-0.25);
    await expect.poll(chips).toContain(`−${formatSeconds(0.25 * kept)}`);
  });
});
