// The toolbar keeps its controls in place when a mode changes (AD-26): switching the display mode,
// time base, layout or trigger mode moves and resizes nothing that shows before and after, the
// changed control included, and the controls keep one order (#355).
import { afterAll, describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { valueNames } from "../../../protocol/src/setting-values.ts";
import { settings, type SettingName } from "../../../protocol/src/settings.ts";
import { byId, settle, startApp, type Started } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

type State = Partial<Record<SettingName, string>>;

// The toolbar's children that show, by name (an id, or a class and its place among its kind),
// with their left edge and width. The spacer is left out: it takes up whatever room is left.
function measure(): Map<string, { left: number; width: number }> {
  const seen = new Map<string, number>();
  const boxes = new Map<string, { left: number; width: number }>();
  for (const child of byId("toolbar").children) {
    const element = child as HTMLElement;
    const kind = element.className;
    const count = seen.get(kind) ?? 0;
    seen.set(kind, count + 1);
    if (element.classList.contains("spacer")) {
      continue;
    }
    const box = element.getBoundingClientRect();
    const style = getComputedStyle(element);
    if (box.width === 0 || style.visibility === "hidden") {
      continue;
    }
    boxes.set(element.id || `${kind}#${String(count)}`, {
      left: box.left,
      width: box.width,
    });
  }
  return boxes;
}

async function show(started: Started, state: State): Promise<void> {
  for (const [name, value] of Object.entries(state)) {
    started.deliver({ type: "stateChanged", name, value });
  }
  await settle();
}

const pairsOf = (values: readonly string[]) =>
  values.flatMap((from) =>
    values.filter((to) => to !== from).map((to) => [from, to] as const),
  );

const timeBases = valueNames(settings.timeBase.values);
const layouts = valueNames(settings.viewLayout.values);
const triggerModes = valueNames(settings.triggerMode.values);

// Each switch a user makes, from one state to the other, in the states it applies in.
const switches: { name: string; from: State; to: State }[] = [
  {
    name: "display mode Single to Multi",
    from: { displayMode: "single" },
    to: { displayMode: "multi" },
  },
  {
    name: "display mode Multi to Single",
    from: { displayMode: "multi" },
    to: { displayMode: "single" },
  },
  {
    name: "Compare off to on, in Multi",
    from: { displayMode: "multi", compare: "off" },
    to: { compare: "on" },
  },
  ...pairsOf(timeBases).map(([from, to]) => ({
    name: `time base ${from} to ${to}`,
    from: { displayMode: "single", timeBase: from },
    to: { timeBase: to },
  })),
  ...["single", "multi"].flatMap((displayMode) =>
    pairsOf(layouts).map(([from, to]) => ({
      name: `layout ${from} to ${to}, in ${displayMode}`,
      from: { displayMode, viewLayout: from },
      to: { viewLayout: to },
    })),
  ),
  ...pairsOf(triggerModes).map(([from, to]) => ({
    name: `trigger mode ${from} to ${to}`,
    from: { displayMode: "single", timeBase: "trigger", triggerMode: from },
    to: { triggerMode: to },
  })),
];

const widths = [minimumEditorWidth, defaultEditorWidth];

describe.each(widths)("the toolbar at %i px", (width) => {
  test.each(switches)(
    "moves and resizes nothing on $name",
    async ({ from, to }) => {
      await page.viewport(width, 600);
      const started = await startApp();
      await show(started, { compare: "off", ...from });
      const before = measure();
      await show(started, to);
      const after = measure();
      const both = [...before.keys()].filter((key) => after.has(key));
      expect(both.length).toBeGreaterThan(5);
      for (const key of both) {
        expect(after.get(key)?.left, key).toBeCloseTo(
          before.get(key)?.left ?? NaN,
          1,
        );
        expect(after.get(key)?.width, key).toBeCloseTo(
          before.get(key)?.width ?? NaN,
          1,
        );
      }
    },
  );

  // The order #355 sets: the Views slot after the mode, Layout after it, then the time base.
  const order = [
    "brand#0",
    "mode-group",
    "views-slot",
    "layout-group",
    "divider#0",
    "time-base-group",
    "time-group",
    "vertical-group",
    "trigger-divider",
    "trigger-group",
    "arm",
    "freeze",
    "auto-freeze",
  ];
  test.each([
    { displayMode: "single", timeBase: "beats" },
    { displayMode: "multi", timeBase: "beats" },
    { displayMode: "single", timeBase: "trigger", triggerMode: "single" },
  ])("keeps its order in $displayMode, $timeBase", async (state) => {
    await page.viewport(width, 600);
    const started = await startApp();
    await show(started, state);
    const shown = [...measure().entries()]
      .sort(([, a], [, b]) => a.left - b.left)
      .map(([key]) => key);
    expect(shown).toEqual(order.filter((key) => shown.includes(key)));
    expect(shown).toContain("views-slot");
  });

  // A drop-down's arrow sits at its right edge, whatever the value; Single's Views and Multi's
  // Views (group) are one width, in one place.
  test.each(switches)(
    "keeps each drop-down's arrow at its right edge, before and after $name",
    async ({ from, to }) => {
      await page.viewport(width, 600);
      const started = await startApp();
      for (const state of [{ compare: "off", ...from }, to]) {
        await show(started, state);
        const buttons = [
          ...document.querySelectorAll<HTMLElement>(
            ".field-button, .readout-tag:not([aria-disabled='true'])",
          ),
        ].filter((button) => button.getBoundingClientRect().width > 0);
        expect(buttons.length).toBeGreaterThan(3);
        for (const button of buttons) {
          const chevron = button.querySelector(".chevron");
          if (chevron === null) {
            continue;
          }
          const box = button.getBoundingClientRect();
          const style = getComputedStyle(button);
          const inset =
            parseFloat(style.paddingRight) + parseFloat(style.borderRightWidth);
          expect(
            box.right - inset - chevron.getBoundingClientRect().right,
            button.id,
          ).toBeLessThan(1);
        }
      }
    },
  );

  test("gives Single's Views and Multi's Views one width", async () => {
    await page.viewport(width, 600);
    const started = await startApp();
    await show(started, { displayMode: "single" });
    const single = byId("views-details").getBoundingClientRect();
    await show(started, { displayMode: "multi" });
    const multi = byId("group-details").getBoundingClientRect();
    expect(multi.left).toBeCloseTo(single.left, 1);
    expect(multi.width).toBeCloseTo(single.width, 1);
  });
});
