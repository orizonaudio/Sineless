// A span longer than the capture keeps, on the real engine's frames: the host prepared the
// instance at 192 kHz and then ran it at 384 kHz, so the capture keeps its 192 kHz size. A 2 s span
// fits what it keeps; 4 s does not, and the axis shows the span the frame says it shows.
import { beforeAll, describe, expect, test } from "vitest";

import { timeLabels as axisTimeLabels } from "../../src/view/axis-labels.ts";
import { parseQuantity } from "../../src/view/format.ts";
import { fullView } from "../../src/view/zoom.ts";
import { byId, chips, settle, startRecorded, typeInto } from "./harness.ts";

// The time labels in seconds, in screen order.
function labelSeconds(): number[] {
  return [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.firstChild?.textContent ?? "")
    .filter((text) => text.endsWith("s"))
    .map((text) => parseQuantity(text, "time") ?? Number.NaN);
}

// Whether the labels mark a rolling span of `span` seconds, within the labels' three digits.
function marksSpan(span: number): boolean {
  const expected = axisTimeLabels(span, true, fullView).map((label) =>
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

describe("a recorded span against a capture prepared at half the rate", () => {
  beforeAll(async () => {
    await startRecorded("LimitedSpan_384kAfter192k");
  });

  test("shows a span the capture keeps whole, unlimited", async () => {
    await settle();
    await expect.poll(() => marksSpan(2)).toBe(true);
    expect(chips()).not.toContain("Span limited");
  });

  test("shows what the capture keeps of a longer span, and says so", async () => {
    await typeInto("span-field", "4 s");
    await expect.poll(chips).toContain("Span limited");
    await settle();
    // The engine decides what is kept; the frame says it.
    const kept = window.orizonDebug?.lastFrame?.spanSeconds ?? 0;
    expect(kept).toBeGreaterThan(2);
    expect(kept).toBeLessThan(4);
    await expect.poll(() => marksSpan(kept)).toBe(true);
  });
});
