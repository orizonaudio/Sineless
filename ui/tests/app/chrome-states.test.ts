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
    // An inset line along the top: no sideways offset, a downward one.
    expect(shadows[0]).toMatch(/\) 0px [\d.]*[1-9][\d.]*px .*inset$/u);
  });

  // A drag field's up-down cue ends where a menu's chevron stroke does, in the same place in a
  // group: inside one, at its end, and alone.
  const chevronEnd = (menu: HTMLElement) => {
    const chevron = menu.querySelector<SVGSVGElement>(".chevron");
    const path = chevron?.querySelector("path");
    if (!chevron || !path) {
      throw new Error("no chevron");
    }
    const box = chevron.getBoundingClientRect();
    const scale = box.width / chevron.viewBox.baseVal.width;
    const stroke = parseFloat(getComputedStyle(path).strokeWidth);
    const right =
      (path.getBBox().x + path.getBBox().width + stroke / 2) * scale;
    return menu.getBoundingClientRect().right - (box.left + right);
  };
  const cueEnd = (field: HTMLElement) =>
    parseFloat(getComputedStyle(field).paddingRight);
  test.each([
    ["inside a group", "time-group", "views-details"],
    ["at a group's end", "vertical-group", "layout-details"],
  ] as const)(
    "a drag field's cue ends where a menu's chevron does, %s",
    async (_, field, menu) => {
      await startApp();
      await settle();
      expect(
        Math.abs(cueEnd(byId(field)) - chevronEnd(byId(menu))),
      ).toBeLessThanOrEqual(0.5);
    },
  );
});

describe("the focus ring on a switch that is on", () => {
  // A switch that is on, or a chosen option, is filled in the ring's own light colour, so the ring
  // goes inside it with a band of the bars' colour between: it shows under keyboard focus whichever
  // kind of pressed button it is on.
  const pressed: readonly [string, () => HTMLElement][] = [
    [
      "a switch in the bars, on",
      () => {
        const button = byId("meter-toggle");
        button.setAttribute("aria-pressed", "true");
        return button;
      },
    ],
    [
      "a chosen option in a popover",
      () =>
        byId("mode-popover").querySelector<HTMLElement>(
          'button[aria-pressed="true"]',
        ) ?? byId("mode-popover"),
    ],
    [
      "Preview fix, on",
      () => {
        const label = document.createElement("div");
        label.className = "lane-label";
        const button = document.createElement("button");
        button.className = "preview-fix";
        button.textContent = "Preview fix";
        button.setAttribute("aria-pressed", "true");
        label.append(button);
        byId("lane-labels").append(label);
        return button;
      },
    ],
  ];
  test.each(pressed)("%s keeps the band inside its ring", async (_, find) => {
    await startApp();
    await settle();
    byId("mode-popover").hidden = false;
    const button = find();
    // A key press makes the next focus a keyboard one, whatever the pointer did before.
    await userEvent.keyboard("{Tab}");
    button.focus();
    expect(button.matches(":focus-visible")).toBe(true);
    const caseRgb = getComputedStyle(document.body).backgroundColor;
    const style = getComputedStyle(button);
    const ring = {
      style: style.outlineStyle,
      width: parseFloat(style.outlineWidth),
      offset: parseFloat(style.outlineOffset),
      colour: style.outlineColor,
    };
    // The band: an inset shadow in the bars' colour with no offset or blur, as wide as its spread.
    const band = style.boxShadow
      .split(/,(?![^(]*\))/u)
      .map((shadow) => shadow.trim())
      .filter(
        (shadow) => shadow.startsWith(caseRgb) && shadow.endsWith("inset"),
      )
      .map((shadow) => {
        const lengths = shadow
          .slice(caseRgb.length)
          .trim()
          .split(/\s+/u)
          .slice(0, 4)
          .map(parseFloat);
        return lengths[0] === 0 && lengths[1] === 0 && lengths[2] === 0
          ? (lengths[3] ?? 0)
          : 0;
      });
    const fill = over(style.backgroundColor, caseColour());
    button.blur();
    byId("mode-popover").hidden = true;
    // A ring drawn inside the button,
    expect(ring.style).toBe("solid");
    expect(ring.width).toBeGreaterThan(0);
    expect(ring.offset).toBeLessThanOrEqual(0);
    // with a band that reaches further in than the ring does, so it parts the ring from the fill,
    expect(Math.max(0, ...band)).toBeGreaterThan(-ring.offset);
    // and the ring standing out from the band.
    const ringRgb = over(ring.colour, fill);
    expect(contrast(ringRgb, caseColour())).toBeGreaterThanOrEqual(
      stateContrast,
    );
  });
});
