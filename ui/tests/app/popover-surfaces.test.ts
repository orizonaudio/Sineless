// What sits over the screen reads as what it is (#409): the readouts are part of the instrument,
// drawn in the bars' colour rather than as menus; a button in a popover is drawn like the popover's
// tracks, so it stands off the popover; every text in a popover is legible; and a switch on a lane
// label reads as on against off.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  caseColour,
  contrast,
  gaps,
  opaque,
  over,
  type Rgb,
} from "./colour.ts";
import {
  byId,
  choice,
  press,
  setCompare,
  settle,
  startApp,
} from "./harness.ts";

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

  // Under the pointer it lightens as a popover's options do, not to a bar control's hover fill.
  test("a button in a popover lightens as the popover's options do", async () => {
    await startApp();
    await settle();
    await userEvent.click(byId("about-details"));
    const contact = byId("contact");
    await userEvent.hover(contact);
    const probe = document.createElement("div");
    probe.style.background = "var(--sl-track-hover)";
    document.body.append(probe);
    const optionHover = getComputedStyle(probe).backgroundColor;
    probe.remove();
    expect(getComputedStyle(contact).backgroundColor).toBe(optionHover);
  });

  // Every popover's text: row names, options chosen and not, number values, the version (a guard:
  // the lighter popover and tracks keep their text at 4.5:1).
  test.each([
    "mode-popover",
    "views-popover",
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

describe("popover choices", () => {
  // A choice's track and a number field stand off the popover with the bars' glass rim, so they
  // read as controls, not as tinted patches.
  test.each([
    "mode-popover",
    "views-popover",
    "layout-popover",
    "time-base-popover",
    "trigger-popover",
    "grid-popover",
  ])("%s draws its choices and fields with the bars' rim", async (id) => {
    await startApp();
    await settle();
    const rim = getComputedStyle(byId("layout-details")).boxShadow;
    expect(rim).not.toBe("none");
    const popover = byId(id);
    const hidden = [popover, ...ancestorsHidden(popover)];
    for (const node of hidden) {
      node.hidden = false;
    }
    const controls = [...popover.querySelectorAll(".segmented, .field")];
    const drawn = controls.map(
      (control) => getComputedStyle(control).boxShadow,
    );
    for (const node of hidden) {
      node.hidden = true;
    }
    expect(controls.length).toBeGreaterThan(0);
    for (const shadow of drawn) {
      expect(shadow).toBe(rim);
    }
  });

  // An option that cannot be chosen recedes in the row names' grey under its dim, rather than
  // reading as a white option that is merely faint; one that is chosen keeps the thumb's dark label,
  // legible on the light thumb. Every way an option is unavailable: Source's views that are off,
  // and R, M and S on a mono track. (In Multi the view toggles' row is hidden, not unavailable.)
  test.each([
    ["Source, its views off", "trigger-popover", "source", 2],
    ["the view toggles on a mono track", "views-popover", "views", 1],
  ] as const)(
    "%s: unavailable options read as unavailable, a chosen one stays legible",
    async (_, popoverId, groupId, inputChannels) => {
      await startApp({ inputChannels });
      await settle();
      const popover = byId(popoverId);
      const hidden = [popover, ...ancestorsHidden(popover)];
      for (const node of hidden) {
        node.hidden = false;
      }
      const buttons = [...byId(groupId).querySelectorAll("button")];
      const name = popover.querySelector(".popover-row > span");
      const grey = getComputedStyle(name ?? document.body).color;
      const chosen = getComputedStyle(
        byId("mode-popover").querySelector('button[aria-pressed="true"]') ??
          document.body,
      ).color;
      const looks = buttons
        .filter((button) => button.disabled)
        .map((button) => ({
          pressed: button.getAttribute("aria-pressed") === "true",
          colour: getComputedStyle(button).color,
        }));
      for (const node of hidden) {
        node.hidden = true;
      }
      expect(looks.some((look) => !look.pressed)).toBe(true);
      for (const look of looks) {
        expect(look.colour).toBe(look.pressed ? chosen : grey);
      }
    },
  );
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

describe("joined channels", () => {
  // Channels can have several chosen. Chosen neighbours read as a bar group: apart by a group's inner
  // gap, their facing corners a group's inner corners, their outer ends a thumb's full radius; and
  // choosing or dropping a channel moves none of them. Every channel on, and L, R and S on with M
  // off, so a join and a lone chosen channel both show.
  test.each([
    ["every channel on", ["M", "S"]],
    ["L, R and S on, M off", ["S"]],
  ] as const)("%s: chosen neighbours read as a bar group", async (_, extra) => {
    await startApp();
    await settle();
    const channels = () => [...byId("views").querySelectorAll("button")];
    // Each channel's place in its track, wherever the popover sits.
    const lefts = () =>
      channels().map(
        (button) =>
          button.getBoundingClientRect().left -
          byId("views").getBoundingClientRect().left,
      );
    const popover = byId("views-popover");
    const hidden = [popover, ...ancestorsHidden(popover)];
    for (const node of hidden) {
      node.hidden = false;
    }
    const before = lefts();
    for (const node of hidden) {
      node.hidden = true;
    }
    for (const label of extra) {
      await press(choice("views", label));
    }
    await settle();
    for (const node of hidden) {
      node.hidden = false;
    }
    const after = lefts();
    const barGap = gaps([byId("time-base-details"), byId("time-group")])[0];
    const barInner = getComputedStyle(byId("time-group")).borderTopLeftRadius;
    const chosen = channels().filter(
      (button) => button.getAttribute("aria-pressed") === "true",
    );
    const looks = chosen.map((button) => ({
      box: button.getBoundingClientRect(),
      style: getComputedStyle(button),
      joinedLeft:
        button.previousElementSibling?.getAttribute("aria-pressed") === "true",
      joinedRight:
        button.nextElementSibling?.getAttribute("aria-pressed") === "true",
    }));
    const thumb = getComputedStyle(
      byId("mode-popover").querySelector('button[aria-pressed="true"]') ??
        document.body,
    ).borderTopLeftRadius;
    for (const node of hidden) {
      node.hidden = true;
    }
    expect(after).toEqual(before);
    expect(looks.filter((look) => look.joinedRight).length).toBeGreaterThan(0);
    for (const [i, look] of looks.entries()) {
      expect(look.style.borderTopLeftRadius).toBe(
        look.joinedLeft ? barInner : thumb,
      );
      expect(look.style.borderTopRightRadius).toBe(
        look.joinedRight ? barInner : thumb,
      );
      const next = looks[i + 1];
      if (look.joinedRight && next) {
        expect(next.box.left - look.box.right).toBeCloseTo(barGap ?? NaN, 1);
      }
    }
  });
});

describe("lane labels", () => {
  // A label's buttons sit concentric in its chip (the chip's corner less its padding is theirs),
  // as the bars' thumbs in their tracks; Preview fix keeps the lane buttons' side padding, so it is
  // a capsule of the label's size. Multi with Compare on, where the labels carry buttons.
  test("a lane label's buttons sit concentric in its chip", async () => {
    await startApp();
    await settle();
    await press(choice("display-mode", "Multi"));
    await setCompare(true);
    await settle();
    const px = (value: string) => parseFloat(value);
    const chips = [
      ...byId("lane-labels").querySelectorAll<HTMLElement>(
        ".lane-label > span",
      ),
    ].filter((chip) => chip.querySelector("button:not(.preview-fix)"));
    expect(chips.length).toBeGreaterThan(0);
    for (const chip of chips) {
      const style = getComputedStyle(chip);
      for (const button of chip.querySelectorAll("button:not(.preview-fix)")) {
        expect(px(getComputedStyle(button).borderTopLeftRadius)).toBe(
          px(style.borderTopLeftRadius) - px(style.paddingTop),
        );
      }
    }
    const fix = byId("lane-labels").querySelector(".preview-fix");
    const check = byId("lane-labels").querySelector(".compare-check");
    expect(fix).not.toBeNull();
    const side = getComputedStyle(check ?? document.body).paddingRight;
    expect(getComputedStyle(fix ?? document.body).paddingLeft).toBe(side);
    expect(getComputedStyle(fix ?? document.body).paddingRight).toBe(side);
  });

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
