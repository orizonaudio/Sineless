// Beats' grid as the page draws it from the mock host's synced frames: the Time step moves the
// lines one ladder level (bars, beats, sixteenths in a 4-bar sweep of 4/4), Triplets draws thirds of
// the beat with labels that read them, Trig's and Free's steps never reach Beats, and Fine's guard
// measures the screen in CSS pixels on a Retina display. Lines are counted on the page's own
// canvas, not worked out again from the geometry that draws them.
import { afterAll, describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import {
  gridFineBeatLineGap,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { mockBarQuarterNotes } from "../../src/bridge/mock-transport.ts";
import { beforeApp, byId, settle, startApp, type Started } from "./harness.ts";

async function set(
  app: Started,
  name: SettingName,
  value: SettingValue,
): Promise<void> {
  app.transport.send({ type: "setSetting", name, value });
  await settle(6);
}

function tokenRgb(name: string): number[] {
  const hex = getComputedStyle(document.documentElement)
    .getPropertyValue(name)
    .trim()
    .replace("#", "");
  return [0, 2, 4].map((offset) => parseInt(hex.slice(offset, offset + 2), 16));
}

// The vertical lines crossing a row just under the top lane's top, each with whether it is drawn
// in the axis colour (strong).
function verticalLines(): { strong: boolean }[] {
  const canvas = byId("grid") as HTMLCanvasElement;
  const context = canvas.getContext("2d");
  const y = Math.round(3 * window.devicePixelRatio);
  const row = context?.getImageData(0, y, canvas.width, 1).data;
  const axis = tokenRgb("--sl-grid-strong");
  const lines: { strong: boolean }[] = [];
  // Each run of drawn pixels, judged by its most covered pixel, which is wholly covered.
  let densest = -1;
  const close = () => {
    if (densest >= 0) {
      const at = densest * 4;
      lines.push({
        strong: [0, 1, 2].every(
          (channel) => row?.[at + channel] === axis[channel],
        ),
      });
    }
    densest = -1;
  };
  for (let x = 0; x < canvas.width; x++) {
    const alpha = row?.[x * 4 + 3] ?? 0;
    if (alpha <= 127) {
      close();
    } else if (densest < 0 || alpha > (row?.[densest * 4 + 3] ?? 0)) {
      densest = x;
    }
  }
  close();
  return lines;
}

function labelTexts(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")].map(
    (label) => label.firstChild?.textContent ?? "",
  );
}

// A new instance starts in Beats.
describe("in Beats, a 4-bar sweep of 4/4", () => {
  test.each([
    // Coarse: bars; Medium: beats; Fine: sixteenths.
    ["coarse", 4 - 1],
    ["medium", 4 * mockBarQuarterNotes - 1],
    ["fine", 4 * 4 * mockBarQuarterNotes - 1],
  ] as const)("the %s Time step draws %i lines", async (time, count) => {
    expect(mockBarQuarterNotes).toBe(4);
    const app = await startApp();
    // Wide enough that Fine's sixteenths keep their gap.
    const sixteenths = 4 * 4 * mockBarQuarterNotes;
    expect(byId("screen").clientWidth / sixteenths).toBeGreaterThanOrEqual(
      gridFineBeatLineGap,
    );
    await set(app, "beatSpan", "fourBars");
    await set(app, "gridTimeBeats", time);
    expect(verticalLines()).toHaveLength(count);
  });

  test("Trig's and Free's Time steps leave Beats' grid as it is", async () => {
    const app = await startApp();
    await set(app, "beatSpan", "fourBars");
    await set(app, "gridTimeTrigger", "fine");
    await set(app, "gridTimeFree", "coarse");
    expect(verticalLines()).toHaveLength(15);
  });
});

test("Triplets draws a quarter bar in sixths, strong on the thirds, labelled in thirds", async () => {
  const app = await startApp();
  await set(app, "beatSpan", "quarterBar");
  await set(app, "beatDivision", "triplets");
  const lines = verticalLines();
  expect(lines).toHaveLength(5);
  expect(lines.map((line) => line.strong)).toEqual([
    false,
    true,
    false,
    true,
    false,
  ]);
  expect(labelTexts()).toContain("+1/3");
  expect(labelTexts()).toContain("+2/3");
});

// The page's screen is as wide in CSS pixels on a Retina display as on a 1x one; its canvas has
// twice the pixels. In the narrowest editor, a 6-bar sweep of 6/4 draws beats at Medium, and
// sixteenths at Fine would stand under 8 CSS pixels apart (twice that in device pixels): closer
// than the guard, so Fine draws the beats. (In 4/4 no view puts them that close at this width.)
describe("on a display at twice the pixel ratio", () => {
  const ratio = Object.getOwnPropertyDescriptor(window, "devicePixelRatio");
  beforeApp(() => {
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      get: () => 2,
    });
  });
  afterAll(() => {
    if (ratio !== undefined) {
      Object.defineProperty(window, "devicePixelRatio", ratio);
    }
  });

  test("Fine keeps Medium's beats where its sixteenths would crowd in CSS pixels", async () => {
    await page.viewport(minimumEditorWidth, 600);
    const bar = 6;
    const app = await startApp({ barQuarterNotes: bar });
    await set(app, "beatSpan", "sixBars");
    await set(app, "gridTimeBeats", "fine");
    const beats = 6 * bar;
    const sixteenthCss = (0.25 / beats) * byId("screen").clientWidth;
    expect(sixteenthCss).toBeLessThan(gridFineBeatLineGap);
    expect(sixteenthCss * window.devicePixelRatio).toBeGreaterThanOrEqual(
      gridFineBeatLineGap,
    );
    // Medium's beats inside a bar-aligned sweep, the five bar lines strong.
    const lines = verticalLines();
    expect(lines).toHaveLength(beats - 1);
    expect(lines.filter((line) => line.strong)).toHaveLength(5);
  });
});
