// The status bar never moves (AD-26, DESIGN 1.5): switching the display mode (the Channel button
// then names a letter or "TT"), Compare, the Meter, the hover or a freeze (which brings
// chips) moves and resizes none of its parts, and all of them stay inside the window, at the
// minimum width and the default.
import { afterAll, describe, expect, test } from "vitest";
import { page } from "vitest/browser";

import {
  defaultEditorWidth,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
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
    ].entries()) {
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
