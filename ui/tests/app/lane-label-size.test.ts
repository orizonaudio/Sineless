// A lane's label keeps its size and place when Compare turns on or off: the colour key is a square
// the size and place of Compare's check box, in a box the size of the check box's button, so the
// names do not jump at each switch.
import { describe, expect, test } from "vitest";

import { byId, setCompare, settle, showDisplay, startApp } from "./harness.ts";

// Each lane label's name: where its text starts, and the height of its line.
function names(): { left: number; height: number; text: string }[] {
  return [...byId("lane-labels").querySelectorAll(".lane-label")].map(
    (label) => {
      const holder =
        label.querySelector<HTMLElement>(".compare-check") ??
        (label.firstElementChild as HTMLElement);
      const text = [...holder.childNodes].find(
        (node) => node.nodeType === Node.TEXT_NODE && node.textContent !== "",
      );
      const range = document.createRange();
      if (text !== undefined) {
        range.selectNodeContents(text);
      }
      // The label's own box: with Compare on it holds the check box's button.
      const box = label.firstElementChild as HTMLElement;
      return {
        left: range.getBoundingClientRect().left,
        height: box.getBoundingClientRect().height,
        text: text?.textContent ?? "",
      };
    },
  );
}

describe("lane labels", () => {
  test("keep their names' place and height when Compare turns on and off", async () => {
    await startApp();
    await showDisplay("Multi");
    await settle();
    const off = names();
    expect(off.length).toBeGreaterThan(1);
    await setCompare(true);
    await settle();
    const on = names();
    await setCompare(false);
    await settle();
    const back = names();
    // Compare adds the sum's lane; the tracks' labels are compared by name.
    for (const measured of [on, back]) {
      for (const before of off) {
        const after = measured.find(({ text }) => text === before.text);
        expect(after, before.text).toBeDefined();
        expect(after?.left).toBeCloseTo(before.left, 0);
        expect(after?.height).toBeCloseTo(before.height, 0);
      }
    }
  });
});
