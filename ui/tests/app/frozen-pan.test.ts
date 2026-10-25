// A frozen Seconds view pans back through the held audio, no further than the engine says it
// reaches, and its chip says how far; when the span grows the reach shrinks, and the pan with it.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { formatSeconds } from "../../src/view/format.ts";
import {
  byId,
  choice,
  chips,
  settle,
  shownChips,
  startApp,
  swipe,
  typeInto,
} from "./harness.ts";

const panChip = (seconds: number) => `−${formatSeconds(seconds)}`;
// How far back the latest frame says the frozen view reaches.
const reach = () => window.orizonDebug?.lastFrame?.panReach ?? NaN;

// A fresh freeze of a 100 ms Seconds view.
async function freezeAt100Ms(): Promise<void> {
  await userEvent.click(choice("time-base", "Sec"));
  await typeInto("span-field", "100 ms");
  await expect
    .element(byId("span-field").querySelector(".field") as HTMLElement)
    .toHaveAttribute("aria-valuetext", "100 ms");
  if (byId("freeze").getAttribute("aria-pressed") === "true") {
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
  }
  await userEvent.click(byId("freeze"));
  await settle();
  await expect.poll(chips).toEqual(["Frozen"]);
}

// The Frozen chip, in the accent, and the button that says so.
function expectFrozenShown(): void {
  expect(byId("freeze").textContent.trim()).toBe("Frozen");
  const chip = byId("chips").querySelector(".chip");
  expect(chip?.textContent).toBe("Frozen");
  expect(chip?.classList.contains("frozen")).toBe(true);
}

describe("a frozen view", () => {
  beforeAll(async () => {
    await startApp();
  });

  test("pans back by swipes, within the held audio", async () => {
    await freezeAt100Ms();
    expect(reach()).toBeGreaterThan(1);

    swipe(-2); // Two screens back: 200 ms.
    await expect.poll(chips).toEqual(["Frozen", panChip(0.2)]);
    // Both fit the status bar: neither folds into "+n".
    expect(shownChips()).toEqual(["Frozen", panChip(0.2)]);
    expect(byId("chips").querySelector(".chip-more")).toBeNull();
    // The offset is a plain state: neither the accent nor the problem colour.
    expect(byId("chips").children[1]?.className).toBe("chip");

    swipe(-1000); // Far beyond the hold: as far as it reaches.
    await settle();
    await expect.poll(chips).toEqual(["Frozen", panChip(reach())]);

    swipe(1000); // Forward again, no further than the freeze.
    await expect.poll(chips).toEqual(["Frozen"]);
  });

  test("keeps the pan within the reach when the span grows", async () => {
    await freezeAt100Ms();
    swipe(-1000);
    await settle();
    const before = reach();
    await expect.poll(chips).toEqual(["Frozen", panChip(before)]);
    await typeInto("span-field", "1 s");
    await settle();
    expect(reach()).toBeLessThan(before);
    await expect.poll(chips).toEqual(["Frozen", panChip(reach())]);
  });

  test("starts unpanned when frozen again", async () => {
    await freezeAt100Ms();
    expectFrozenShown();
    swipe(-2);
    await expect.poll(chips).toEqual(["Frozen", panChip(0.2)]);
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).not.toContain("Frozen");
    expect(byId("freeze").textContent.trim()).toBe("Freeze");
    await userEvent.click(byId("freeze"));
    await settle();
    await expect.poll(chips).toEqual(["Frozen"]);
    expectFrozenShown();
  });
});
