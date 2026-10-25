// A mono track: the engine draws L alone, and the page offers only L.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import { byId, choice, settle, startApp } from "./harness.ts";

describe("a mono track", () => {
  beforeAll(async () => {
    await startApp({ inputChannels: 1 });
  });

  test("offers L alone among the views", async () => {
    const toggles = [...byId("views").querySelectorAll("button")];
    expect(toggles.map((button) => button.textContent)).toEqual([
      "L",
      "R",
      "M",
      "S",
    ]);
    await expectOnlyLeftEnabled(toggles);
  });

  test("labels the readouts Left, whatever the trigger source", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await expect.poll(() => byId("readout-tag").textContent).toBe("Left");
    await expect.poll(() => byId("views-summary").textContent).toBe("L");
  });

  test("draws one lane when stacked, with the trigger marker in it", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("layout", "Stack"));
    await settle();
    expect([...(window.orizonDebug?.lastFrame?.views.keys() ?? [])]).toEqual([
      ChannelView.left,
    ]);
    // One lane: the marker sits within the whole screen's height, at the trigger level 0, the
    // middle.
    const screen = byId("screen").getBoundingClientRect();
    const marker = byId("trigger-marker").getBoundingClientRect();
    const middle = marker.top + marker.height / 2 - screen.top;
    expect(middle / screen.height).toBeCloseTo(0.5, 1);
  });
});

async function expectOnlyLeftEnabled(
  toggles: HTMLButtonElement[],
): Promise<void> {
  const [left, ...others] = toggles;
  await expect.element(left as HTMLButtonElement).toBeEnabled();
  for (const button of others) {
    await expect.element(button).toBeDisabled();
  }
}
