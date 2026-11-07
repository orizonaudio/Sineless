// The grid's Type as the page draws it: None leaves each lane's zero line; Cross hair, in Trig, the
// zero lines and the trigger point's line, with ticks where the time and level lines would be; the
// labels stay those Grid shows; and Cross hair held for another time base (a project edited by
// hand) draws Grid there. Lines are counted on the page's own canvas.
import { describe, expect, test } from "vitest";

import { defaultTriggerPosition as triggerPosition } from "../../../protocol/src/limits.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { byId, inTimeBase, settle, startApp, type Started } from "./harness.ts";

async function set(
  app: Started,
  name: SettingName,
  value: SettingValue,
): Promise<void> {
  app.transport.send({ type: "setSetting", name, value });
  await settle();
}

function grid(): { width: number; height: number; data: Uint8ClampedArray } {
  const canvas = byId("grid") as HTMLCanvasElement;
  const image = canvas
    .getContext("2d")
    ?.getImageData(0, 0, canvas.width, canvas.height);
  return {
    width: canvas.width,
    height: canvas.height,
    data: image?.data ?? new Uint8ClampedArray(),
  };
}

// The drawn runs along a row (CSS pixels from the top), each with its left edge in CSS pixels.
function runsAlongRow(cssY: number): number[] {
  const { width, data } = grid();
  const ratio = window.devicePixelRatio;
  const y = Math.round(cssY * ratio);
  const starts: number[] = [];
  let inside = false;
  for (let x = 0; x < width; x++) {
    const drawn = (data[(y * width + x) * 4 + 3] ?? 0) > 127;
    if (drawn && !inside) {
      starts.push(x / ratio);
    }
    inside = drawn;
  }
  return starts;
}

// The drawn runs down a column (CSS pixels from the left), over the top lane's upper half.
function runsDownColumn(cssX: number, laneCount: number): number {
  const { width, height, data } = grid();
  const x = Math.round(cssX * window.devicePixelRatio);
  const zero = Math.floor(height / (2 * laneCount));
  let count = 0;
  let inside = false;
  for (let y = 0; y < zero - 2; y++) {
    const drawn = (data[(y * width + x) * 4 + 3] ?? 0) > 127;
    count += drawn && !inside ? 1 : 0;
    inside = drawn;
  }
  return count;
}

function labelTexts(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")].map(
    (label) => label.firstChild?.textContent ?? "",
  );
}

describe("in Trig", () => {
  test("None draws the zero lines alone and keeps Grid's labels", async () => {
    const app = await startApp();
    await inTimeBase("Trig");
    await settle();
    const labels = labelTexts();
    await set(app, "gridTypeTrigger", "none");
    expect(runsAlongRow(3)).toEqual([]);
    expect(runsDownColumn(3, 2)).toBe(0);
    expect(labelTexts()).toEqual(labels);
  });

  test("Cross hair draws the trigger point's line, ticks where the lines would be, and Grid's labels", async () => {
    const app = await startApp();
    await inTimeBase("Trig");
    await set(app, "gridTimeTrigger", "coarse");
    await settle();
    const labels = labelTexts();
    await set(app, "gridTypeTrigger", "crossHair");
    const width = byId("screen").clientWidth;
    const height = byId("screen").clientHeight;
    // Under the top lane's top: the trigger point's line alone.
    const across = runsAlongRow(3);
    expect(across).toHaveLength(1);
    expect(across[0]).toBeCloseTo(triggerPosition * width, 0);
    // Just above the top lane's zero line: the line and a tick at each of Coarse's 4 divisions.
    const zero = height / 4;
    expect(runsAlongRow(zero - 2)).toHaveLength(1 + 4);
    // Away from the line and the zero line: nothing.
    expect(runsAlongRow(zero - 12)).toHaveLength(1);
    // Down the line's side, its ticks at each of Medium's 3 dB lines; nowhere else any.
    expect(runsDownColumn(triggerPosition * width + 2, 2)).toBe(5);
    expect(runsDownColumn(width / 2, 2)).toBe(0);
    expect(labelTexts()).toEqual(labels);
    // The dashed trigger line still marks the trigger point.
    expect(byId("trigger-line").hidden).toBe(false);
  });
});

// Cross hair marks the trigger point, which only Trig has.
test.each([
  ["Free", "gridTypeFree"],
  ["Beats", "gridTypeBeats"],
] as const)(
  "Cross hair held for %s draws Grid there",
  async (timeBase, setting) => {
    const app = await startApp();
    await set(app, "beatSpan", "twoBars");
    await inTimeBase(timeBase);
    await settle(4);
    const lines = runsAlongRow(3);
    await set(app, setting, "crossHair");
    await settle(4);
    expect(runsAlongRow(3)).toEqual(lines);
    expect(lines.length).toBeGreaterThan(1);
  },
);
