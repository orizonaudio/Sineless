// A track whose audio holds samples that are not finite (NaN or infinite), as from a broken plugin
// upstream: the page warns of them in every display mode, and names the lanes that hold them.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, choice, settle, startApp } from "./harness.ts";

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
  probe.style.color = "var(--warn)";
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

  test("names this track's lane in orange, and in Compare the sum's", async () => {
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
});
