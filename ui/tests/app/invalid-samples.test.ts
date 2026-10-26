// A track whose audio holds samples that are not finite (NaN or infinite), as from a broken plugin
// upstream: the page warns of them in every display mode, and names the lanes that hold them.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  byId,
  chips,
  choice,
  settle,
  startApp,
  swipe,
  typeInto,
} from "./harness.ts";

const chipTexts = () =>
  [...byId("chips").children].map((chip) => chip.textContent);

// The lane names marked as holding samples that are not finite.
const markedLanes = () =>
  [...byId("lane-labels").querySelectorAll(".lane-label > span")]
    .filter((name) => name.classList.contains("invalid"))
    .map((name) => name.textContent);

// The colour each marked lane's name is drawn in: in Compare, a track's name is its check box's.
const markedColours = () =>
  [...byId("lane-labels").querySelectorAll(".lane-label > span.invalid")].map(
    (name) =>
      getComputedStyle(name.querySelector(".compare-check") ?? name).color,
  );

// The warning colour, as the page computes it.
const warnColour = () => {
  const probe = document.createElement("span");
  probe.style.color = "var(--sl-warn)";
  document.body.append(probe);
  const colour = getComputedStyle(probe).color;
  probe.remove();
  return colour;
};

describe("a track sending samples that are not finite", () => {
  beforeAll(async () => {
    await startApp({ brokenSamples: 3 });
  });

  test.each(["Single", "Multi", "Compare"])(
    "warns of them in %s",
    async (mode) => {
      await userEvent.click(choice("display-mode", mode));
      await settle();
      expect(chipTexts()).toContain("Invalid samples");
    },
  );

  test("names this track's lane in the warning colour, and in Compare the sum's", async () => {
    await userEvent.click(choice("display-mode", "Multi"));
    await userEvent.click(choice("layout", "Stack"));
    await settle();
    expect(markedLanes()).toEqual(["Kick"]);
    expect(markedColours()).toEqual([warnColour()]);

    await userEvent.click(choice("display-mode", "Compare"));
    await settle();
    const marked = markedLanes();
    expect(marked).toHaveLength(2);
    expect(marked[0]).toBe("Kick");
    expect(marked[1]).toMatch(/^Sum/u);
    expect(markedColours()).toEqual([warnColour(), warnColour()]);
  });

  // More states than the status bar's fixed area holds: invalid samples, frozen and panned back.
  // The first, the problem, stays shown in yellow; the rest fold into one "+n" chip that still
  // lists them, and nothing runs outside the area, so the readouts beside it never move.
  test("folds the states that don't fit into one chip that lists them", async () => {
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Sec"));
    await typeInto("span-field", "100 ms");
    if (byId("freeze").getAttribute("aria-pressed") !== "true") {
      await userEvent.click(byId("freeze"));
    }
    // Panned back once frozen, which adds the offset's chip.
    await expect.poll(chips).toContain("Frozen");
    swipe(-0.5);
    await expect.poll(() => chips().length).toBe(3);
    await expect.poll(chips).toContain("Invalid samples");
    const states = chips();
    expect(states[0]).toBe("Invalid samples");

    const host = byId("chips");
    const more = host.querySelector<HTMLElement>(".chip-more");
    expect(more, "the +n chip").not.toBeNull();
    const folded = [...(more as HTMLElement).querySelectorAll(".chip")].map(
      (chip) => chip.textContent,
    );
    expect(folded.length).toBeGreaterThan(0);
    expect((more as HTMLElement).firstChild?.textContent).toBe(
      `+${String(folded.length)}`,
    );
    // Every state is shown or listed once, in order, the problem among the shown.
    const shown = [...host.children].filter((child) => child !== more);
    expect([...shown.map((child) => child.textContent), ...folded]).toEqual(
      states,
    );
    expect(shown[0]?.classList.contains("problem")).toBe(true);
    // For screen readers, "+n" says how many are folded and names each, and none of those shown.
    const label = more?.getAttribute("aria-label") ?? "";
    expect(label).toContain(String(folded.length));
    for (const state of folded) {
      expect(label).toContain(state);
    }
    for (const child of shown) {
      expect(label).not.toContain(child.textContent);
    }
    const area = host.getBoundingClientRect();
    for (const child of host.children) {
      const box = child.getBoundingClientRect();
      expect(box.left).toBeGreaterThanOrEqual(area.left - 0.5);
      expect(box.right).toBeLessThanOrEqual(area.right + 0.5);
    }
    // Resting the pointer on "+n" shows the folded states, whole, inside the window.
    const list = (more as HTMLElement).querySelector<HTMLElement>(".chip-list");
    expect(list, "the folded list").not.toBeNull();
    await expect.element(list as HTMLElement).not.toBeVisible();
    await userEvent.hover(more as HTMLElement);
    await expect.element(list as HTMLElement).toBeVisible();
    const box = (list as HTMLElement).getBoundingClientRect();
    expect(box.left).toBeGreaterThanOrEqual(0);
    expect(box.top).toBeGreaterThanOrEqual(0);
    expect(box.right).toBeLessThanOrEqual(window.innerWidth);
    expect(box.bottom).toBeLessThanOrEqual(window.innerHeight);
    for (const chip of (list as HTMLElement).querySelectorAll(".chip")) {
      await expect.element(chip as HTMLElement).toBeVisible();
    }
    await userEvent.unhover(more as HTMLElement);
    await userEvent.click(byId("freeze"));
  });
});
