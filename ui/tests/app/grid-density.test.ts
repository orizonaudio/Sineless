// The grid's Time and Level steps as the page draws them in Trig and Free: Time divides the screen
// into its equal divisions, Level draws its level lines and labels, each time base keeps its own
// steps, and the time labels stay on the fifths at every Time step. Lines are counted on the page's
// own canvas, not worked out again from the geometry that draws them.
import { describe, expect, test } from "vitest";

import { GridDensity } from "../../../protocol/src/enums.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import type { GridDensityName } from "../../src/view/grid-settings.ts";
import { byId, inTimeBase, settle, startApp, type Started } from "./harness.ts";

const densities = Object.keys(GridDensity) as GridDensityName[];

// Equal divisions at each Time step, from the issue (#364): 5, 10 and 20.
const divisions: Readonly<Record<GridDensityName, number>> = {
  coarse: 5,
  medium: 10,
  fine: 20,
};

const timeBases = [
  { label: "Trig", suffix: "Trigger" },
  { label: "Free", suffix: "Free" },
] as const;

// Sets a setting as the engine would on the toolbar's request.
async function set(
  app: Started,
  name: SettingName,
  value: GridDensityName,
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

describe.each(timeBases)("in $label", ({ label, suffix }) => {
  test.each(densities)(
    "the %s Time step divides the screen as the issue says, with time labels on its fifths",
    async (density) => {
      const app = await startApp();
      await inTimeBase(label);
      await set(app, `gridTime${suffix}` as SettingName, density);
      expect(verticalLines()).toBe(divisions[density] - 1);
      const positions = timeLabelPositions();
      expect(positions).toHaveLength(4);
      positions.forEach((position, index) => {
        expect(position).toBeCloseTo((index + 1) / 5, 2);
      });
    },
  );

  test("Level's steps draw 6 dB, 3 dB, then 1 dB lines, and label 6 dB or 3 dB", async () => {
    const app = await startApp();
    await inTimeBase(label);
    const counts = new Map<GridDensityName, number>();
    const labels = new Map<GridDensityName, string[]>();
    for (const density of densities) {
      await set(app, `gridLevel${suffix}` as SettingName, density);
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
});

test("each time base keeps its own Time and Level steps", async () => {
  const app = await startApp();
  await inTimeBase("Trig");
  await set(app, "gridTimeTrigger", "fine");
  await set(app, "gridLevelTrigger", "coarse");
  expect(verticalLines()).toBe(19);
  expect(levelLinesInTopLane(2)).toBe(3);

  await inTimeBase("Free");
  await settle();
  expect(verticalLines()).toBe(9);
  expect(levelLinesInTopLane(2)).toBe(5);
  await set(app, "gridTimeFree", "coarse");
  expect(verticalLines()).toBe(4);

  await inTimeBase("Trig");
  await settle();
  expect(verticalLines()).toBe(19);
  expect(levelLinesInTopLane(2)).toBe(3);
});
