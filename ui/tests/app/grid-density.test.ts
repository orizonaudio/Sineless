// The grid's Density as the page draws it in Trig and Free: one choice divides the screen into its
// equal divisions and draws its level lines and labels at once, in every display and layout; a
// Density chosen in one time base is drawn in every other, and the time labels stay on the fifths
// at every Density. Lines are counted on the page's own canvas, not worked out again from the
// geometry that draws them.
import { describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import { GridDensity } from "../../../protocol/src/enums.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import type { GridDensityName } from "../../src/view/grid-settings.ts";
import {
  byId,
  displays,
  inTimeBase,
  settle,
  showDisplay,
  startApp,
  type Started,
} from "./harness.ts";

const densities = Object.keys(GridDensity) as GridDensityName[];

// Equal divisions at each Density, from the issues (#364, #407): 5, 10 and 20.
const divisions: Readonly<Record<GridDensityName, number>> = {
  coarse: 5,
  medium: 10,
  fine: 20,
};

const timeBases = [{ label: "Trig" }, { label: "Free" }] as const;

// Sets a setting as the engine would on the toolbar's request.
async function set(
  app: Started,
  name: SettingName,
  value: string,
): Promise<void> {
  app.transport.send({ type: "setSetting", name, value });
  await settle();
}

function gridImage(): {
  width: number;
  height: number;
  data: Uint8ClampedArray;
} {
  const canvas = byId("grid") as HTMLCanvasElement;
  const context = canvas.getContext("2d");
  expect(context).not.toBeNull();
  const image = context?.getImageData(0, 0, canvas.width, canvas.height);
  return {
    width: canvas.width,
    height: canvas.height,
    data: image?.data ?? new Uint8ClampedArray(),
  };
}

// The number of separate lines a run of pixels crosses: runs of drawn pixels.
function runs(alphas: readonly number[]): number {
  let count = 0;
  let inside = false;
  for (const alpha of alphas) {
    const drawn = alpha > 127;
    if (drawn && !inside) {
      count++;
    }
    inside = drawn;
  }
  return count;
}

// Vertical lines, counted along a row just under the top lane's top, above its first level line
// below the top.
function verticalLines(): number {
  const { width, data } = gridImage();
  const y = Math.round(3 * window.devicePixelRatio);
  return runs(
    Array.from({ length: width }, (_, x) => data[(y * width + x) * 4 + 3] ?? 0),
  );
}

// Level lines in the top half of the top lane, the top line included, counted down a column left
// of the first vertical line.
function levelLinesInTopLane(laneCount: number): number {
  const { width, height, data } = gridImage();
  const x = 3;
  const zero = Math.floor(height / (2 * laneCount));
  return runs(
    Array.from(
      { length: zero - 2 },
      (_, y) => data[(y * width + x) * 4 + 3] ?? 0,
    ),
  );
}

function axisTexts(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")].map(
    (label) => label.firstChild?.textContent ?? "",
  );
}

function timeLabelPositions(): number[] {
  const width = byId("screen").clientWidth;
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")]
    .filter((label) => label.style.bottom !== "")
    .map((label) => (parseFloat(label.style.left) - 4) / width);
}

describe.each(timeBases)("in $label", ({ label }) => {
  test.each(densities)(
    "the %s Density divides the screen as the issue says, with time labels on its fifths",
    async (density) => {
      const app = await startApp();
      await inTimeBase(label);
      await set(app, "gridDensity", density);
      expect(verticalLines()).toBe(divisions[density] - 1);
      const positions = timeLabelPositions();
      expect(positions).toHaveLength(4);
      positions.forEach((position, index) => {
        expect(position).toBeCloseTo((index + 1) / 5, 2);
      });
    },
  );

  test("the Density's level steps draw 6 dB, 3 dB, then 1 dB lines, and label 6 dB or 3 dB", async () => {
    const app = await startApp();
    await inTimeBase(label);
    const counts = new Map<GridDensityName, number>();
    const labels = new Map<GridDensityName, string[]>();
    for (const density of densities) {
      await set(app, "gridDensity", density);
      counts.set(density, levelLinesInTopLane(2));
      labels.set(density, axisTexts());
    }
    // At the test page's height each lane fits every 3 dB line, and Fine adds 1 dB lines.
    expect(counts.get("coarse")).toBe(3);
    expect(counts.get("medium")).toBe(5);
    expect(counts.get("fine")).toBeGreaterThan(5);
    const minus = "−";
    expect(labels.get("coarse")).toContain(`${minus}6`);
    expect(labels.get("coarse")).not.toContain(`${minus}3`);
    expect(labels.get("coarse")).not.toContain(`${minus}9`);
    expect(labels.get("medium")).toContain(`${minus}3`);
    expect(labels.get("fine")).toEqual(labels.get("medium"));
  });

  // In every display and layout, one Density moves both steps: the time divisions and the level
  // lines of the top lane, which thin from Fine to Coarse.
  describe.each(displays)("%s", (display) => {
    test.each(["overlaid", "stacked"])(
      "%s, each Density sets the time and the level step together",
      async (layout) => {
        // Tall enough that Compare's five stacked lanes keep Medium's 3 dB lines apart from
        // Coarse's 6 dB ones (shorter lanes thin them to the same lines).
        await page.viewport(1000, 1400);
        const app = await startApp();
        await inTimeBase(label);
        await showDisplay(display);
        await set(app, "viewLayout", layout);
        const lanes =
          layout === "stacked"
            ? (window.orizonDebug?.lastFrame?.lanes.length ?? 0)
            : 1;
        const laneCount = Math.max(lanes, 1);
        const levels = new Map<GridDensityName, number>();
        for (const density of densities) {
          await set(app, "gridDensity", density);
          expect(verticalLines(), density).toBe(divisions[density] - 1);
          levels.set(density, levelLinesInTopLane(laneCount));
        }
        expect(levels.get("coarse")).toBeLessThan(levels.get("medium") ?? 0);
        expect(levels.get("medium")).toBeLessThanOrEqual(
          levels.get("fine") ?? 0,
        );
      },
    );
  });
});

test("a Density chosen in one time base is drawn in every other", async () => {
  // Each Density's level lines, as Trig draws them: the reference every other time base matches.
  const app = await startApp();
  await inTimeBase("Trig");
  const levels = new Map<GridDensityName, number>();
  for (const density of densities) {
    await set(app, "gridDensity", density);
    levels.set(density, levelLinesInTopLane(2));
  }
  expect(levels.get("fine")).not.toBe(levels.get("coarse"));

  // Chosen in Beats, a new instance's time base: four bars of 4/4 at Fine draw sixteenths, where
  // Medium draws beats, with Fine's level lines.
  await inTimeBase("Beats");
  await set(app, "beatSpan", "fourBars");
  await set(app, "gridDensity", "fine");
  expect(verticalLines()).toBe(4 * 4 * 4 - 1);
  expect(levelLinesInTopLane(2)).toBe(levels.get("fine"));

  await inTimeBase("Trig");
  await settle();
  expect(verticalLines()).toBe(divisions.fine - 1);
  expect(levelLinesInTopLane(2)).toBe(levels.get("fine"));

  await inTimeBase("Free");
  await settle();
  expect(verticalLines()).toBe(divisions.fine - 1);
  expect(levelLinesInTopLane(2)).toBe(levels.get("fine"));
  await set(app, "gridDensity", "coarse");

  await inTimeBase("Trig");
  await settle();
  expect(verticalLines()).toBe(divisions.coarse - 1);
  expect(levelLinesInTopLane(2)).toBe(levels.get("coarse"));
});
