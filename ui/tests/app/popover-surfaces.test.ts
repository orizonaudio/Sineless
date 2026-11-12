// What sits over the screen reads as what it is (#409): the readouts are part of the instrument,
// drawn in the bars' colour rather than as menus; a button in a popover is drawn like the popover's
// tracks, so it stands off the popover; every text in a popover is legible; and a switch on a lane
// label reads as on against off.
import { describe, expect, test } from "vitest";

import { caseColour, contrast, opaque, over, type Rgb } from "./colour.ts";
import { byId, settle, startApp } from "./harness.ts";

// WCAG 2.2: text 4.5:1 (1.4.3), a state shown by a fill 3:1 (1.4.11).
const textContrast = 4.5;
const stateContrast = 3;

// The colour an element's text sits on: every background from the opaque popover (or the bar)
// down to the element, laid over each other.
function backdrop(element: Element, base: Element): Rgb {
  const chain: Element[] = [];
  for (let node: Element | null = element; node; node = node.parentElement) {
    chain.unshift(node);
    if (node === base) {
      break;
    }
  }
  return chain.reduce<Rgb>(
    (under, node) => over(getComputedStyle(node).backgroundColor, under),
    caseColour(),
  );
}

describe("readouts", () => {
  test("the cursor and hover readouts are drawn in the bars' colour, alike, not as popovers", async () => {
    await startApp();
    await settle();
    const popover = getComputedStyle(byId("mode-popover"));
    const [cursor, hover] = ["cursor-readout", "hover-readout"].map((id) =>
      getComputedStyle(byId(id)),
    );
    for (const readout of [cursor, hover]) {
      expect(opaque(readout?.backgroundColor ?? "")).toEqual(caseColour());
      expect(readout?.backgroundColor).not.toBe(popover.backgroundColor);
      expect(readout?.boxShadow).toBe("none");
    }
    expect(hover?.borderTopColor).toBe(cursor?.borderTopColor);
    expect(hover?.borderTopLeftRadius).toBe(cursor?.borderTopLeftRadius);
  });
});

describe("popovers", () => {
  // About's vendor button is drawn like a popover's track, not like a bar control.
  test("a button in a popover is drawn like the popover's tracks", async () => {
    await startApp();
    await settle();
    const contact = getComputedStyle(byId("contact"));
    const track = byId("mode-popover").querySelector(".segmented");
    expect(track).not.toBeNull();
    expect(contact.backgroundColor).toBe(
      getComputedStyle(track ?? document.body).backgroundColor,
    );
    expect(contact.backgroundColor).not.toBe(
      getComputedStyle(byId("meter-toggle")).backgroundColor,
    );
  });

  // Every popover's text: row names, options chosen and not, number values, the version (a guard:
  // the lighter popover and tracks keep their text at 4.5:1).
  test.each([
    "mode-popover",
    "views-popover",
    "group-popover",
    "layout-popover",
    "time-base-popover",
    "trigger-popover",
    "grid-popover",
    "about-popover",
  ])("%s keeps its text legible", async (id) => {
    await startApp();
    await settle();
    const popover = byId(id);
    const hidden = [popover, ...ancestorsHidden(popover)];
    for (const node of hidden) {
      node.hidden = false;
    }
    const texts = [
      ...popover.querySelectorAll<HTMLElement>(
        ".popover-title, .popover-row > span, button, .field, #about-version",
      ),
    ].filter(
      (element) =>
        element.textContent.trim() !== "" &&
        !(element instanceof HTMLButtonElement && element.disabled),
    );
    expect(texts.length).toBeGreaterThan(1);
    for (const element of texts) {
      const under = backdrop(element, popover);
      const ink = over(getComputedStyle(element).color, under);
      expect(
        contrast(ink, under),
        `${element.id || element.className}: ${element.textContent.trim()}`,
      ).toBeGreaterThanOrEqual(textContrast);
    }
    for (const node of hidden) {
      node.hidden = true;
    }
  });
});

function ancestorsHidden(element: HTMLElement): HTMLElement[] {
  const hidden: HTMLElement[] = [];
  for (let node = element.parentElement; node; node = node.parentElement) {
    if (node.hidden) {
      hidden.push(node);
    }
  }
  return hidden;
}

describe("lane labels", () => {
  // Preview fix, on a Compare sum lane, is a switch like the bars': on reads as on against off.
  test("Preview fix reads as on against off, its label legible in both", async () => {
    await startApp();
    await settle();
    const label = document.createElement("div");
    label.className = "lane-label";
    const name = document.createElement("span");
    const button = document.createElement("button");
    button.className = "preview-fix";
    button.textContent = "Preview fix";
    name.append(button);
    label.append(name);
    byId("lane-labels").append(label);
    const look = () => {
      const fill = backdrop(button, byId("screen"));
      return { fill, ink: over(getComputedStyle(button).color, fill) };
    };
    button.setAttribute("aria-pressed", "false");
    const off = look();
    button.setAttribute("aria-pressed", "true");
    const on = look();
    label.remove();
    expect(contrast(on.fill, off.fill)).toBeGreaterThanOrEqual(stateContrast);
    expect(contrast(off.ink, off.fill)).toBeGreaterThanOrEqual(textContrast);
    expect(contrast(on.ink, on.fill)).toBeGreaterThanOrEqual(textContrast);
  });
});
