// The grid's Type as the page draws it (#384), in every time base, display and layout: Grid draws
// the time and level lines and the labels, Ticks the labels alone, None neither; each lane's zero
// line stays in every type, and so does the trigger point's line. Lines are counted on the page's
// own canvas.
import { describe, expect, test } from "vitest";

import { GridType } from "../../../protocol/src/enums.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import type { GridTypeName } from "../../src/view/grid-settings.ts";
import {
  byId,
  displays,
  inTimeBase,
  settle,
  showDisplay,
  startApp,
  type Started,
} from "./harness.ts";

const types = Object.keys(GridType) as GridTypeName[];

// What each type shows, from the issue's table.
const shows: Readonly<
  Record<GridTypeName, { readonly lines: boolean; readonly labels: boolean }>
> = {
  none: { lines: false, labels: false },
  ticks: { lines: false, labels: true },
  grid: { lines: true, labels: true },
};

const timeBases = ["Trig", "Free", "Beats"] as const;
const layouts = ["overlaid", "stacked"] as const;

async function set(
  app: Started,
  name: SettingName,
  value: SettingValue,
): Promise<void> {
  app.transport.send({ type: "setSetting", name, value });
  await settle(4);
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

function runs(alphas: readonly number[]): number {
  let count = 0;
  let inside = false;
  for (const alpha of alphas) {
    const drawn = alpha > 127;
    count += drawn && !inside ? 1 : 0;
    inside = drawn;
  }
  return count;
}

// Vertical lines, along a row just under the top, above any level line below the top.
function verticalLines(): number {
  const { width, data } = grid();
  const y = Math.round(3 * window.devicePixelRatio);
  return runs(
    Array.from({ length: width }, (_, x) => data[(y * width + x) * 4 + 3] ?? 0),
  );
}

// Horizontal lines down a column near the left edge, left of any vertical line: the zero lines,
// and the level lines where drawn.
function horizontalLines(): number {
  const { width, height, data } = grid();
  return runs(
    Array.from(
      { length: height },
      (_, y) => data[(y * width + 3) * 4 + 3] ?? 0,
    ),
  );
}

// The axis labels shown: level labels by their text, time labels by their place, since Beats'
// texts move on with each cycle of the sweep.
function labelTexts(): string[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")].map(
    (label) =>
      label.style.top !== ""
        ? `level ${label.textContent}`
        : `time at ${label.style.left}`,
  );
}

const triggerLine = () => byId("trigger-line");

describe.each(displays)("in %s", (display) => {
  describe.each(layouts)("%s", (layout) => {
    test.each(timeBases)(
      "in %s, each type draws its lines and labels, the zero lines and the trigger line stay",
      async (timeBase) => {
        const app = await startApp();
        await set(app, "beatSpan", "twoBars");
        await showDisplay(display);
        await set(app, "viewLayout", layout);
        await inTimeBase(timeBase);
        await settle(4);
        const seen = new Map<
          GridTypeName,
          { vertical: number; horizontal: number; labels: string[] }
        >();
        // Grid first and again last: choosing another type and back redraws what it removed.
        for (const type of ["grid", ...types, "grid"] as const) {
          await set(app, "gridType", type);
          const now = {
            vertical: verticalLines(),
            horizontal: horizontalLines(),
            labels: labelTexts(),
          };
          expect(now.vertical > 0, `${type} vertical lines`).toBe(
            shows[type].lines,
          );
          expect(now.labels.length > 0, `${type} labels`).toBe(
            shows[type].labels,
          );
          // In Trig the trigger point's line shows in every type; elsewhere it never does.
          expect(triggerLine().hidden, `${type} trigger line`).toBe(
            timeBase !== "Trig",
          );
          const first = seen.get(type);
          if (first === undefined) {
            seen.set(type, now);
          } else {
            expect(now, `${type} again`).toEqual(first);
          }
        }
        const none = seen.get("none");
        const ticks = seen.get("ticks");
        const full = seen.get("grid");
        // Each lane's zero line stays without lines; Grid adds its level lines to them.
        expect(none?.horizontal).toBeGreaterThan(0);
        expect(ticks?.horizontal).toBe(none?.horizontal);
        expect(full?.horizontal).toBeGreaterThan(none?.horizontal ?? 0);
        // Ticks shows Grid's labels.
        expect(ticks?.labels).toEqual(full?.labels);
      },
    );
  });
});

test("under None in Trig the trigger point's line still follows the Position", async () => {
  const app = await startApp();
  await inTimeBase("Trig");
  await set(app, "gridType", "none");
  const before = triggerLine().style.left;
  await set(app, "triggerPosition", 0.2);
  await expect
    .poll(() => window.orizonDebug?.lastFrame?.triggerPosition)
    .toBeCloseTo(0.2, 6);
  await settle();
  expect(triggerLine().hidden).toBe(false);
  expect(triggerLine().style.left).not.toBe(before);
  expect(Number.parseFloat(triggerLine().style.left)).toBeCloseTo(20, 1);
});

function levelLabels(): string[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")]
    .filter((label) => label.style.top !== "")
    .map((label) => label.textContent);
}

function timeLabels(): string[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")]
    .filter((label) => label.style.bottom !== "")
    .map((label) => label.textContent);
}

describe("with Ticks, the labels follow the steps", () => {
  test.each(timeBases)(
    "in %s, Level moves the level labels",
    async (timeBase) => {
      const app = await startApp();
      await inTimeBase(timeBase);
      await set(app, "gridType", "ticks");
      await set(app, "gridLevel", "medium");
      const medium = levelLabels();
      await set(app, "gridLevel", "coarse");
      expect(levelLabels()).not.toEqual(medium);
      expect(levelLabels().length).toBeGreaterThan(0);
    },
  );

  test("in Beats, Time moves the time labels", async () => {
    const app = await startApp();
    // A bar: Medium labels beats, Coarse the bar alone.
    await set(app, "beatSpan", "oneBar");
    await set(app, "gridType", "ticks");
    const medium = timeLabels();
    await set(app, "gridTime", "coarse");
    expect(timeLabels()).not.toEqual(medium);
    expect(timeLabels().length).toBeGreaterThan(0);
  });

  test.each(["Trig", "Free"] as const)(
    "in %s the time labels stay on the fifths at every Time step",
    async (timeBase) => {
      const app = await startApp();
      await inTimeBase(timeBase);
      await set(app, "gridType", "ticks");
      const medium = timeLabels();
      expect(medium).toHaveLength(4);
      for (const time of ["coarse", "fine"]) {
        await set(app, "gridTime", time);
        expect(timeLabels(), time).toEqual(medium);
      }
    },
  );
});
