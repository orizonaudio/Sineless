// The bottom-left corner of the screen (#393): every time label sits 4 px right of its line, the
// first one on the left edge included, and the level labels that would reach into the band the
// time labels take at the bottom are left out. The band is the screen's full width, so a level
// label never comes and goes as Beats' labels move under a pan.
import { describe, expect, test } from "vitest";

import {
  DisplayMode,
  GridType,
  TimeBase,
  ViewLayout,
} from "../../../protocol/src/enums.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { amplitudeLabels } from "../../src/view/axis-labels.ts";
import { choiceLabels } from "../../src/view/choice-labels.ts";
import { formatDb } from "../../src/view/format.ts";
import {
  drawsLabels,
  type GridTypeName,
} from "../../src/view/grid-settings.ts";
import {
  byId,
  inTimeBase,
  settle,
  showDisplay,
  startApp,
  type Started,
} from "./harness.ts";

type TimeBaseName = keyof typeof TimeBase;
type LayoutName = keyof typeof ViewLayout;

const displayModes = Object.keys(DisplayMode).map(
  (mode) => choiceLabels.displayMode[mode as keyof typeof DisplayMode],
) as ("Single" | "Multi")[];
const layouts = Object.keys(ViewLayout) as LayoutName[];
const timeBases = Object.keys(TimeBase) as TimeBaseName[];
const labelledTypes = (Object.keys(GridType) as GridTypeName[]).filter(
  drawsLabels,
);

async function set(
  app: Started,
  name: SettingName,
  value: SettingValue,
): Promise<void> {
  app.transport.send({ type: "setSetting", name, value });
  await settle(4);
}

function axisLabels(): HTMLElement[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")];
}

// Level labels are placed from the top, time labels from the bottom.
const levelLabels = () =>
  axisLabels().filter((label) => label.style.top !== "");
const timeLabels = () =>
  axisLabels().filter((label) => label.style.bottom !== "");
const levelTexts = () => levelLabels().map((label) => label.textContent);

// The top of the band the time labels take, in page coordinates.
function bandTop(): number {
  return Math.min(
    ...timeLabels().map((label) => label.getBoundingClientRect().top),
  );
}

function expectClearOfBand(): void {
  expect(levelLabels().length).toBeGreaterThan(0);
  expect(timeLabels().length).toBeGreaterThan(0);
  const top = bandTop();
  for (const label of levelLabels()) {
    expect(
      label.getBoundingClientRect().bottom,
      `level label ${label.textContent}`,
    ).toBeLessThanOrEqual(top);
  }
}

// The level labels the shared helper places for the screen as it is now.
function expectedLevelTexts(layout: LayoutName, lanes: number): string[] {
  return amplitudeLabels(
    0,
    lanes,
    ViewLayout[layout],
    byId("labels").clientHeight,
  ).map((label) => label.text);
}

async function show(
  app: Started,
  display: "Single" | "Multi",
  layout: LayoutName,
  timeBase: TimeBaseName,
  type: GridTypeName,
): Promise<void> {
  await set(app, "beatSpan", "twoBars");
  await showDisplay(display);
  await set(app, "viewLayout", layout);
  await inTimeBase(choiceLabels.timeBase[timeBase]);
  await set(app, "gridType", type);
  await settle(4);
}

describe.each(displayModes)("in %s", (display) => {
  describe.each(layouts)("%s", (layout) => {
    describe.each(timeBases)("in %s", (timeBase) => {
      test.each(labelledTypes)(
        "with %s, no level label reaches into the time labels, and each time label sits at its line",
        async (type) => {
          const app = await startApp();
          await show(app, display, layout, timeBase, type);
          expectClearOfBand();
          // Overlaid, the one lane keeps its top label and its zero line's.
          if (layout === "overlaid") {
            const texts = levelTexts();
            expect(texts[0]).toBe(expectedLevelTexts(layout, 1)[0]);
            expect(texts).toContain("−∞");
          }
          const width = byId("labels").clientWidth;
          if (timeBase === "beats") {
            // The full view of a cycle that starts on a bar: its first label is on the left edge.
            expect(timeLabels()[0]?.style.left).toBe("4px");
          } else {
            // The fifths of the screen.
            const lefts = timeLabels().map((label) =>
              Number.parseFloat(label.style.left),
            );
            expect(lefts).toHaveLength(4);
            lefts.forEach((left, index) => {
              expect(left).toBeCloseTo(((index + 1) / 5) * width + 4, 3);
            });
          }
        },
      );
    });
  });
});

// The trigger level marker sits on the left edge in Trig; at the lowest level it reaches the
// bottom corner, which no label then covers.
describe.each(displayModes)("in %s, Trig", (display) => {
  test.each(layouts)(
    "%s, at the lowest trigger level, the marker is clear of every label",
    async (layout) => {
      const app = await startApp();
      await show(app, display, layout, "trigger", "grid");
      await set(app, "triggerLevel", -1);
      const marker = byId("trigger-marker").getBoundingClientRect();
      expect(marker.height).toBeGreaterThan(0);
      for (const label of axisLabels()) {
        const box = label.getBoundingClientRect();
        const apart =
          box.left >= marker.right ||
          box.right <= marker.left ||
          box.top >= marker.bottom ||
          box.bottom <= marker.top;
        expect(apart, `label ${label.textContent}`).toBe(true);
      }
    },
  );
});

test("a short stacked bottom lane drops its zero line's label, the others keep theirs", async () => {
  const tracks = 12;
  const app = await startApp({ trackCount: tracks });
  await show(app, "Multi", "stacked", "beats", "grid");
  await expect
    .poll(() => window.orizonDebug?.lastFrame?.lanes.length)
    .toBe(tracks);
  await settle(4);
  expectClearOfBand();
  const minusInfinity = levelTexts().filter((text) => text === "−∞");
  expect(minusInfinity).toHaveLength(tracks - 1);
});

// A new instance in Beats before the host plays: no time labels, so no band, and every level
// label stays, the bottom-left one included.
describe("with no time labels", () => {
  test.each(layouts)("%s, every level label stays", async (layout) => {
    const app = await startApp({ hostTempo: false });
    await set(app, "viewLayout", layout);
    await set(app, "gridType", "grid");
    expect(timeLabels()).toEqual([]);
    // A frame with no tempo carries no lanes: stacked, one per view shown, each topped by the
    // vertical scale.
    const lanes = levelTexts().filter((text) => text === formatDb(0)).length;
    expect(lanes).toBeGreaterThan(0);
    expect(levelTexts()).toEqual(expectedLevelTexts(layout, lanes));
  });
});
