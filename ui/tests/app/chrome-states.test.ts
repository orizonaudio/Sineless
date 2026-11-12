// What a user can tell apart in the chrome (#409): a control under the pointer changes, and a switch
// that is on reads as on against one that is off, with its label legible either way.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { caseColour, contrast, over } from "./colour.ts";
import { byId, settle, startApp } from "./harness.ts";

// WCAG 2.2: text 4.5:1 (1.4.3), a state shown by a fill 3:1 (1.4.11).
const textContrast = 4.5;
const stateContrast = 3;

const fill = (element: HTMLElement) =>
  over(getComputedStyle(element).backgroundColor, caseColour());
const ink = (element: HTMLElement) =>
  over(getComputedStyle(element).color, fill(element));

describe("the bars' controls", () => {
  // A menu, a drag field and a switch in each bar, and the status bar's round buttons.
  test.each([
    "layout-details",
    "time-base-details",
    "time-group",
    "grid-details",
    "level-cursors",
    "freeze",
    "help",
  ])("%s changes its fill under the pointer", async (id) => {
    await startApp();
    await settle();
    const control = byId(id);
    const rest = getComputedStyle(control).backgroundColor;
    await userEvent.hover(control);
    expect(getComputedStyle(control).backgroundColor).not.toBe(rest);
    await userEvent.hover(byId("wordmark"));
    expect(getComputedStyle(control).backgroundColor).toBe(rest);
  });

  // Every switch in the bars, on and off.
  test.each([
    "meter-toggle",
    "time-cursors",
    "level-cursors",
    "hover-toggle",
    "auto-freeze",
    "freeze",
    "help",
  ])("%s reads as on against off, its label legible in both", async (id) => {
    await startApp();
    await settle();
    const control = byId(id);
    const was = control.getAttribute("aria-pressed");
    control.setAttribute("aria-pressed", "false");
    const off = { fill: fill(control), ink: ink(control) };
    control.setAttribute("aria-pressed", "true");
    const on = { fill: fill(control), ink: ink(control) };
    control.setAttribute("aria-pressed", was ?? "false");
    expect(contrast(on.fill, off.fill)).toBeGreaterThanOrEqual(stateContrast);
    expect(contrast(off.ink, off.fill)).toBeGreaterThanOrEqual(textContrast);
    expect(contrast(on.ink, on.fill)).toBeGreaterThanOrEqual(textContrast);
  });
});

describe("the bars' padding", () => {
  // Every bar control is padded alike: a switch inside its group as a menu inside its group, and at
  // a group's outer end as a menu there, so a group of switches has the rhythm of a group of menus.
  test.each([
    [
      "inside a group",
      "views-details",
      "paddingLeft",
      "time-cursors",
      "paddingLeft",
    ],
    [
      "at a group's start",
      "mode-details",
      "paddingLeft",
      "meter-toggle",
      "paddingLeft",
    ],
    [
      "at a group's end",
      "layout-details",
      "paddingRight",
      "hover-toggle",
      "paddingRight",
    ],
    [
      "at a group's start, toolbar",
      "mode-details",
      "paddingLeft",
      "auto-freeze",
      "paddingLeft",
    ],
  ] as const)(
    "a switch is padded as a menu %s",
    async (_, menu, menuSide, toggle, toggleSide) => {
      await startApp();
      await settle();
      expect(getComputedStyle(byId(toggle))[toggleSide]).toBe(
        getComputedStyle(byId(menu))[menuSide],
      );
    },
  );
});

describe("the bars' edges and cues", () => {
  // A control sits in the bar: its edge is a faint highlight along its top only, with no line all
  // round tracing it against the bar. Bar controls at rest and open, and a popover's track.
  test.each([
    ["a menu", "layout-details", false],
    ["a drag field", "time-group", false],
    ["a switch", "level-cursors", false],
    ["an open menu", "layout-details", true],
    ["a popover's choice", "layout", false],
  ] as const)("%s has a top highlight and no outline", async (_, id, open) => {
    await startApp();
    await settle();
    if (open) {
      await userEvent.click(byId(id));
    }
    const control = byId(id);
    const hidden = [control.closest<HTMLElement>(".popover")].filter(
      (node): node is HTMLElement => node?.hidden === true,
    );
    for (const node of hidden) {
      node.hidden = false;
    }
    const shadows = getComputedStyle(control)
      .boxShadow.split(/,(?![^(]*\))/u)
      .map((shadow) => shadow.trim());
    for (const node of hidden) {
      node.hidden = true;
    }
    expect(shadows).toHaveLength(1);
    // inset, no x offset, 1 px down, no blur, no spread
    expect(shadows[0]).toMatch(/0px 1px 0px 0px inset$/u);
  });
});
