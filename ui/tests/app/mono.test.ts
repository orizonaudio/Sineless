// A mono track: the engine draws L alone, and the page offers only L.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView } from "../../../protocol/src/enums.ts";
import {
  beforeApp,
  byId,
  channelRead,
  choice,
  drawnLetters,
  inTimeBase,
  press,
  settle,
  showViews,
  startApp,
  type Started,
  useReadout,
} from "./harness.ts";

describe("a mono track", () => {
  beforeApp(async () => {
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

  test("reads Left, whatever the trigger source", async () => {
    await press(choice("time-base", "Trig"));
    await expect.poll(channelRead).toBe("Left");
    // The Views field lights L alone, as the frame draws it.
    await expect.poll(drawnLetters).toEqual(["L"]);
    expect(byId("views-details").getAttribute("aria-label")).toBe(
      "Views: L, reads Left",
    );
  });

  test("draws one lane when stacked, with the trigger marker in it", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("layout", "Stack"));
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

// The host changes the track's channels with the editor open: the frames change at once, and the
// snapshot that reports it follows later. The Views field follows the frames.
describe("a track that changes channels", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp({ inputChannels: 2 });
  });

  const lit = drawnLetters;

  test("names the views the frames draw before any snapshot says", async () => {
    await press(choice("display-mode", "Single"));
    await press(choice("time-base", "Trig"));
    await expect.poll(lit).toEqual(["L", "R"]);
    app.transport.changeChannels(1);
    await expect.poll(lit).toEqual(["L"]);
    app.transport.changeChannels(2);
    await expect.poll(lit).toEqual(["L", "R"]);
  });

  // The new count relabels the Views field, the Reads row and the view toggles by itself, with no
  // control touched: a mono track draws and measures L alone, so the Reads row lights L and offers
  // nothing else; stereo again, the channel chosen comes back.
  test("relabels the readouts and the toggles when the snapshot says", async () => {
    const toggles = [...byId("views").querySelectorAll("button")];
    await press(choice("display-mode", "Single"));
    await inTimeBase("Trig");
    await press(choice("source", "R"));
    await showViews(["L", "S"]);
    await useReadout("S");
    await expect.poll(channelRead).toBe("Side");
    await userEvent.click(byId("views-details"));
    await expect.element(byId("views-popover")).toBeVisible();
    app.transport.changeChannels(1, true);
    await expect.poll(channelRead).toBe("Left");
    await expect
      .element(choice("readout-source", "L"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.element(choice("readout-source", "L")).toBeEnabled();
    for (const other of ["R", "M", "S"]) {
      await expect.element(choice("readout-source", other)).toBeDisabled();
    }
    await expectOnlyLeftEnabled(toggles);
    app.transport.changeChannels(2, true);
    await expect.poll(channelRead).toBe("Side");
    for (const letter of ["L", "S"]) {
      await expect.element(choice("readout-source", letter)).toBeEnabled();
    }
    for (const button of toggles) {
      await expect.element(button).toBeEnabled();
    }
    await userEvent.click(byId("views-details"));
    await showViews(["L", "R"]);
    await press(choice("source", "L"));
  });
});
