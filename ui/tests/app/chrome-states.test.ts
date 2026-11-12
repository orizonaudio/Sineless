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
