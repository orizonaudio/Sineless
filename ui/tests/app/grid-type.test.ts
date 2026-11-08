// The grid's Type as the page draws it: None leaves each lane's zero line, in every time base, and
// the labels stay those Grid shows. Lines are counted on the page's own canvas.
import { expect, test } from "vitest";

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

// The drawn runs along a row (CSS pixels from the top).
function runsAlongRow(cssY: number): number {
  const { width, data } = grid();
  const y = Math.round(cssY * window.devicePixelRatio);
  let count = 0;
  let inside = false;
  for (let x = 0; x < width; x++) {
    const drawn = (data[(y * width + x) * 4 + 3] ?? 0) > 127;
    count += drawn && !inside ? 1 : 0;
    inside = drawn;
  }
  return count;
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

test.each([
  ["Trig", "gridTypeTrigger"],
  ["Free", "gridTypeFree"],
  ["Beats", "gridTypeBeats"],
] as const)(
  "in %s, None draws the zero lines alone and keeps Grid's labels",
  async (timeBase, setting) => {
    const app = await startApp();
    await set(app, "beatSpan", "twoBars");
    await inTimeBase(timeBase);
    await settle(4);
    expect(runsAlongRow(3)).toBeGreaterThan(0);
    expect(runsDownColumn(3, 2)).toBeGreaterThan(0);
    const labels = labelTexts();
    await set(app, setting, "none");
    await settle(4);
    expect(runsAlongRow(3)).toBe(0);
    expect(runsDownColumn(3, 2)).toBe(0);
    expect(labelTexts()).toEqual(labels);
  },
);
