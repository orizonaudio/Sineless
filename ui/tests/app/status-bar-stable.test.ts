// The status bar never moves (AD-26, DESIGN 1.5): switching the display mode, Compare, the Meter,
// the hover or a freeze (which brings chips) moves and resizes none of its parts, and all of them stay inside the window, at the
// minimum width and the default.
import { afterAll, describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { gaps } from "./colour.ts";
import { byId, settle, startApp, type Started } from "./harness.ts";

afterAll(async () => {
  await page.viewport(1200, 720);
});

type State = Partial<Record<SettingName, string>>;

// The status bar's parts, by name, with their left edge and width: the left group's controls and
// the right group's, the chips' area as a whole.
function measure(): Map<string, { left: number; width: number }> {
  const boxes = new Map<string, { left: number; width: number }>();
  for (const group of [".status-left", ".status-right"]) {
    for (const [index, child] of [
      ...(document.querySelector(group)?.children ?? []),
    ]
      .flatMap((part) =>
        part.classList.contains("group") ? [...part.children] : [part],
      )
      .entries()) {
      const box = child.getBoundingClientRect();
      boxes.set(`${group} ${child.id || String(index)}`, {
        left: box.left,
        width: box.width,
      });
    }
  }
  return boxes;
}

function rightmost(): number {
  return Math.max(
    ...[...document.querySelectorAll(".statusbar *")].map(
      (element) => element.getBoundingClientRect().right,
    ),
  );
}

async function show(started: Started, state: State): Promise<void> {
  for (const [name, value] of Object.entries(state)) {
    started.deliver({ type: "stateChanged", name, value });
  }
  await settle();
}

const switches: { name: string; from: State; to: State; freeze?: boolean }[] = [
  {
    name: "Single to Multi",
    from: { displayMode: "single" },
    to: { displayMode: "multi" },
  },
  {
    name: "Multi to Single",
    from: { displayMode: "multi" },
    to: { displayMode: "single" },
  },
  {
    name: "Compare on, in Multi",
    from: { displayMode: "multi", compare: "off" },
    to: { compare: "on" },
  },
  { name: "the Meter on", from: { meter: "off" }, to: { meter: "on" } },
  {
    name: "the hover on",
    from: { hoverReadout: "off" },
    to: { hoverReadout: "on" },
  },
  { name: "a freeze", from: {}, to: {}, freeze: true },
];

describe.each([minimumEditorWidth, defaultEditorWidth])(
  "the status bar at %i px",
  (width) => {
    test.each(switches)(
      "moves nothing at $name",
      async ({ from, to, freeze }) => {
        await page.viewport(width, 580);
        const started = await startApp();
        await show(started, from);
        const before = measure();
        await show(started, to);
        if (freeze === true) {
          byId("freeze").click();
          await settle();
          await expect
            .poll(() => byId("chips").textContent)
            .toContain("Frozen");
        }
        expect(measure()).toEqual(before);
        expect(rightmost()).toBeLessThanOrEqual(window.innerWidth);
      },
    );
  },
);

// Concert pitch and the grid shape what the screen draws, so they come first, by the screen's
// left edge; the Meter, the cursors and Hover, which read it, come after them (#380). Each is a
// group of its own, set apart by space alone, as far apart as the toolbar's groups (#409).
test("puts A4 and Grid first, then the Meter, the cursors and Hover, in three groups", async () => {
  await startApp();
  await settle();
  const groups = [...document.querySelectorAll(".status-left > .group")].map(
    (group) => [...group.children],
  );
  expect(
    groups.map((controls) =>
      controls.map((child) => child.id || child.className),
    ),
  ).toEqual([
    ["tool icon-field"],
    ["grid-group"],
    ["meter-toggle", "time-cursors", "level-cursors", "hover-toggle"],
  ]);
  expect(document.querySelector(".status-left > .divider")).toBeNull();
  const inside = gaps(groups.at(-1) ?? []);
  const apart = gaps(
    groups
      .flatMap((controls) => [controls[0], controls.at(-1)])
      .filter((c) => c !== undefined),
  ).filter((_, i) => i % 2 === 1);
  // The toolbar's gap between its first two groups, on the same page.
  const [firstGroup, secondGroup] = [
    ...byId("toolbar").querySelectorAll(":scope > .group"),
  ];
  const toolbarGap = gaps(
    [firstGroup?.lastElementChild, secondGroup?.firstElementChild].filter(
      (c) => c !== null && c !== undefined,
    ),
  )[0];
  expect(Math.max(...inside)).toBeLessThan(Math.min(...apart));
  for (const gap of apart) {
    expect(gap).toBeCloseTo(toolbarGap ?? NaN, 0);
  }
});
