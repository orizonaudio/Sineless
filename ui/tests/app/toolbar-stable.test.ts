// The toolbar keeps its controls in place when a mode changes (AD-26): switching the display mode,
// time base, layout or trigger mode moves and resizes nothing that shows before and after, the
// changed control included, and the controls keep one order (#355), in groups set apart by space
// (#409).
import { afterAll, describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { valueNames } from "../../../protocol/src/setting-values.ts";
import { settings, type SettingName } from "../../../protocol/src/settings.ts";
import { numberFields } from "../../src/view/fields.ts";
import { beatSpanLabels } from "../../src/view/musical-grid.ts";
import { gaps } from "./colour.ts";
import { byId, settle, startApp, type Started } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

type State = Partial<Record<SettingName, string>>;

// The toolbar's controls that show, by name (an id, or a class and its place among its kind),
// with their left edge and width; a group's controls are measured one by one. The spacer is left
// out: it takes up whatever room is left.
function measure(): Map<string, { left: number; width: number }> {
  const seen = new Map<string, number>();
  const boxes = new Map<string, { left: number; width: number }>();
  const controls = [...byId("toolbar").children].flatMap((child) =>
    child.classList.contains("group") ? [...child.children] : [child],
  );
  for (const child of controls) {
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

  // The order #355 sets: the Views slot after the mode, Layout after it, then the time base, the
  // span and the scale, Trigger and Re-arm; Auto-freeze before Freeze (#409).
  const order = [
    "brand#0",
    "mode-group",
    "views-slot",
    "layout-group",
    "time-base-group",
    "time-group",
    "vertical-group",
    "trigger-group",
    "arm",
    "auto-freeze",
    "freeze",
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
          ...document.querySelectorAll<HTMLElement>(".field-button"),
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

// Every value the span pill can show, in any time base: the beat spans' labels, and each step
// Trig's and Free's spans take, coarse and fine, from their shortest to their longest.
function spanLabels(): { host: string; label: string }[] {
  const shown: { host: string; label: string }[] = beatSpanLabels.map(
    (label) => ({ host: "beat-span-field", label }),
  );
  for (const name of ["triggerSpan", "freeSpan"] as const) {
    const field = numberFields[name];
    const { minimum, maximum } = settings[name];
    if (field === undefined) {
      continue;
    }
    for (const fine of [false, true]) {
      for (
        let value: number = minimum, step = 0;
        value <= maximum && step < 1000;
        value = field.step(value, 1, fine), step++
      ) {
        shown.push({ host: field.host, label: field.format(value) });
      }
    }
  }
  return shown;
}

// The span pill is as wide as its longest value: none of them widens it, so a step or a switch of
// time base moves nothing after it.
test("the span pill holds every value its steps can show at one width", async () => {
  await startApp();
  await settle();
  const pill = byId("time-group");
  const width = pill.getBoundingClientRect().width;
  const wider: string[] = [];
  for (const { host, label } of spanLabels()) {
    for (const id of ["span-field", "free-span-field", "beat-span-field"]) {
      byId(id).hidden = id !== host;
    }
    const field = byId(host).querySelector<HTMLElement>(".field");
    if (field === null) {
      continue;
    }
    const before = field.textContent;
    field.textContent = label;
    if (pill.getBoundingClientRect().width > width + 0.5) {
      wider.push(label);
    }
    field.textContent = before;
  }
  expect(wider).toEqual([]);
});

// The toolbar's groups (#409): the view (Mode, Views, Layout), the time (time base, span, scale),
// the trigger (Trigger, Re-arm) and freezing (Auto-freeze, Freeze), set apart by space alone.
const groups = [
  ["mode-group", "views-slot", "layout-group"],
  ["time-base-group", "time-group", "vertical-group"],
  ["trigger-group", "arm"],
  ["auto-freeze", "freeze"],
];

const shownBox = (element: Element): DOMRect | undefined => {
  const box = element.getBoundingClientRect();
  return box.width > 0 && getComputedStyle(element).visibility !== "hidden"
    ? box
    : undefined;
};

// The element that draws a control's shape: the control itself, or its shown button.
function surface(control: Element): HTMLElement {
  if (control.matches("button, .icon-field")) {
    return control as HTMLElement;
  }
  const button = [
    ...control.querySelectorAll<HTMLElement>(":scope .field-button"),
  ].find((candidate) => shownBox(candidate) !== undefined);
  return button ?? (control as HTMLElement);
}

const corner = (element: HTMLElement, side: "left" | "right") => {
  const style = getComputedStyle(element);
  return parseFloat(
    side === "left" ? style.borderTopLeftRadius : style.borderTopRightRadius,
  );
};

const toolbarStates = ["single", "multi"].flatMap((displayMode) =>
  timeBases.flatMap((timeBase) =>
    (timeBase === "trigger" ? triggerModes : ["auto"]).map((triggerMode) => ({
      displayMode,
      timeBase,
      triggerMode,
    })),
  ),
);

describe.each(widths)("the toolbar's groups at %i px", (width) => {
  test.each(toolbarStates)(
    "in $displayMode, $timeBase, $triggerMode: in order, apart, capped at their ends",
    async (state) => {
      await page.viewport(width, 600);
      const started = await startApp();
      await show(started, state);
      const shown = [...byId("toolbar").querySelectorAll(":scope > .group")]
        .filter((group) => shownBox(group) !== undefined)
        .map((group) =>
          [...group.children].filter((child) => shownBox(child) !== undefined),
        );
      // Every shown group holds a shown control, and the groups hold what they should.
      expect(shown.every((controls) => controls.length > 0)).toBe(true);
      expect(
        shown.map((controls) => controls.map((control) => control.id)),
      ).toEqual(
        groups
          .map((ids) => ids.filter((id) => shownBox(byId(id)) !== undefined))
          .filter((ids) => ids.length > 0),
      );
      // Controls in a group are closer than the groups before the spacer are to each other.
      const inside = shown.flatMap((controls) => gaps(controls));
      const apart = shown
        .slice(0, -1)
        .flatMap((controls, i, left) =>
          i === 0
            ? []
            : gaps(
                [left[i - 1]?.at(-1), controls[0]].filter(
                  (c) => c !== undefined,
                ),
              ),
        );
      expect(apart.length).toBeGreaterThan(0);
      expect(Math.max(...inside)).toBeLessThan(Math.min(...apart));
      for (const gap of apart) {
        expect(gap).toBeCloseTo(apart[0] ?? NaN, 0);
      }
      // A group is rounded at its outer ends only; a control alone in its group, on both sides.
      const innerCorner = corner(surface(byId("views-slot")), "left");
      for (const controls of shown) {
        controls.forEach((control, i) => {
          const element = surface(control);
          const isFirst = i === 0;
          const isLast = i === controls.length - 1;
          const name = `${control.id} (${String(i + 1)} of ${String(controls.length)})`;
          expect(corner(element, "left") > innerCorner, name).toBe(isFirst);
          expect(corner(element, "right") > innerCorner, name).toBe(isLast);
        });
      }
    },
  );
});

// Auto-freeze comes before Freeze (#409), live and frozen alike.
test.each([false, true])(
  "puts Auto-freeze before Freeze, frozen: %s",
  async (frozen) => {
    await startApp();
    await settle();
    if (frozen) {
      byId("freeze").click();
      await settle();
    }
    expect(byId("auto-freeze").getBoundingClientRect().right).toBeLessThan(
      byId("freeze").getBoundingClientRect().left,
    );
  },
);

// Freezing the display moves and resizes nothing in the toolbar (AD-26): Freeze reads "Frozen" while
// frozen, and its group sits at the toolbar's right end, so a wider or narrower label would move
// Auto-freeze. Live to frozen and back, at both widths, in each display mode.
describe.each(widths)("freezing, at %i px", (width) => {
  test.each(["single", "multi"])(
    "moves and resizes nothing in %s",
    async (displayMode) => {
      await page.viewport(width, 600);
      const started = await startApp();
      await show(started, { compare: "off", displayMode });
      const live = measure();
      for (const frozen of [true, false]) {
        byId("freeze").click();
        await expect
          .poll(() => byId("freeze").getAttribute("aria-pressed"))
          .toBe(String(frozen));
        await settle();
        const now = measure();
        for (const [key, box] of live) {
          expect(
            now.get(key)?.left,
            `${key}, frozen: ${String(frozen)}`,
          ).toBeCloseTo(box.left, 1);
          expect(
            now.get(key)?.width,
            `${key}, frozen: ${String(frozen)}`,
          ).toBeCloseTo(box.width, 1);
        }
      }
    },
  );
});
